#include "db_database.h"
#include "db_sql_lexer.h"
#include "db_test_directory.h"
#include "db_test_index.h"
#include "db_test_journal.h"

#include <algorithm>
#include <iostream>
#include <map>

namespace
{
    using index_test::require;
    using index_test::same;
    using index_test::Tree;
    using Path = std::filesystem::path;

    template <typename Function>
    void requireError(Function function, const std::string &message)
    {
        try { function(); }
        catch (const std::exception &error)
        {
            require(std::string(error.what()).find(message) != std::string::npos,
                    "Unexpected error: " + std::string(error.what()));
            return;
        }
        throw std::runtime_error("Missing error: " + message);
    }

    auto snapshot(const Path &root)
    {
        std::map<Path, journal_test::Bytes> result;
        for (const auto *directory : {"tables", "indexes"})
            for (const auto &file : std::filesystem::directory_iterator(root / directory))
                result.emplace(Path{directory} / file.path().filename(), journal_test::readFile(file.path()));
        return result;
    }

    std::vector<LocatedRow> readRows(const Path &root)
    {
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        auto table = Table::open(LinuxDirectory(root / "tables"), "items.table", recovery);
        auto cursor = table.scan();
        std::vector<LocatedRow> rows;
        while (auto row = cursor.nextLocated()) rows.push_back(std::move(*row));
        return rows;
    }

    RowId rowIdFor(const Path &root, std::int32_t id)
    {
        for (const auto &row : readRows(root))
            if (std::get<std::int32_t>(row.row.values[0]) == id) return row.id;
        throw std::runtime_error("Missing fixture row");
    }

    void verifyIndexes(const Path &root, const std::vector<std::string> &names)
    {
        const auto rows = readRows(root);
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        for (const auto &name : names)
        {
            Tree tree(root, name, true);
            tree.verifyRouting();
            std::vector<LeafIndexEntry> expected;
            for (const auto &row : rows)
            {
                Row key;
                for (const auto column : tree.metadata.keyColumns)
                    key.values.push_back(row.row.values.at(column));
                expected.push_back({std::move(key), row.id});
            }
            std::sort(expected.begin(), expected.end(), indexEntryLess);
            require(tree.entries.size() == expected.size(), "Index/table entry counts differ: " + name);
            auto index = Index::open(LinuxDirectory(root / "indexes"), name + ".index", recovery);
            auto cursor = index.scan();
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                require(same(tree.entries[i], expected[i]), "Wrong stored key or table RowId: " + name);
                const auto entry = cursor.next();
                require(entry && same(*entry, expected[i]), "Index scan differs from table: " + name);
            }
            require(!cursor.next(), "Index scan returned extra entries");
            for (const auto &entry : expected)
            {
                cursor.seek(entry.key, entry.rowId);
                const auto exact = cursor.next();
                require(exact && same(*exact, entry), "Exact seek failed after mutation");
                const Row prefix{{entry.key.values[0]}};
                cursor.seek(prefix);
                const auto first = cursor.next();
                const auto reference = std::find_if(expected.begin(), expected.end(), [&](const auto &candidate) {
                    return candidate.key.values[0] == prefix.values[0];
                });
                require(first && same(*first, *reference), "Prefix seek skipped a surviving duplicate");
            }
            cursor.seek(Row{});
            require(bool(cursor.next()) == !expected.empty(), "Empty-prefix seek failed after deletion");
        }
    }

    void testUniqueUpdatesAndDeletes()
    {
        TestDirectory directory{"db-index-update-unique"};
        const auto root = directory.path / "db";
        auto db = Database::create(root, "test");
        db.executeSql("CREATE TABLE items (id INT PRIMARY KEY, code INT, label TEXT);");
        db.executeSql("CREATE UNIQUE INDEX by_code ON items (code);");
        db.executeSql("CREATE UNIQUE INDEX by_id ON items (id);");
        db.executeSql("CREATE INDEX composite ON items (code, id);");
        const std::vector<std::string> indexes{"by_code", "by_id", "composite"};
        db.executeSql("INSERT INTO items VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c'), (4, NULL, 'd'), (5, NULL, 'e');");
        require(db.executeSql("UPDATE items SET code = 30 - code WHERE id <= 2;").affectedRows == 2,
                "UPDATE did not report both swapped keys");
        require(db.executeSql("UPDATE items SET code = code;").affectedRows == 5, "Unchanged keys blocked UPDATE");
        verifyIndexes(root, indexes);
        const auto before = snapshot(root);
        requireError([&] { db.executeSql("UPDATE items SET code = 30 WHERE id = 1;"); }, "UNIQUE INDEX");
        require(snapshot(root) == before, "Conflict with unaffected row was not rolled back");
        requireError([&] { db.executeSql("UPDATE items SET code = 99 WHERE id <= 2;"); }, "UNIQUE INDEX");
        require(snapshot(root) == before, "Duplicate replacement keys were not rolled back");
        require(db.executeSql("UPDATE items SET code = 0 WHERE id = 999;").affectedRows == 0, "No-match UPDATE affected rows");
        require(db.executeSql("DELETE FROM items WHERE id = 999;").affectedRows == 0, "No-match DELETE affected rows");
        require(snapshot(root) == before, "No-match mutations changed storage");
        db.executeSql("UPDATE items SET code = NULL WHERE id <= 2;");
        verifyIndexes(root, indexes);
        require(db.executeSql("DELETE FROM items WHERE id = 2;").affectedRows == 1, "DELETE missed a duplicate NULL key");
        db.executeSql("UPDATE items SET code = 10 WHERE id = 1;");
        db.executeSql("INSERT INTO items VALUES (6, 20, 'reused');");
        verifyIndexes(root, indexes);
        db.executeSql("CREATE TABLE children (parent_id INT, FOREIGN KEY (parent_id) REFERENCES items (id));");
        db.executeSql("INSERT INTO children VALUES (1);");
        const auto withChild = snapshot(root);
        requireError([&] { db.executeSql("DELETE FROM items WHERE id = 1;"); }, "FOREIGN KEY");
        requireError([&] { db.executeSql("UPDATE items SET id = 9 WHERE id = 1;"); }, "FOREIGN KEY");
        require(snapshot(root) == withChild, "Rejected FK mutations changed indexes");
        db.executeSql("DELETE FROM children;");
        db.executeSql("DELETE FROM items WHERE id = 1;");
        verifyIndexes(root, indexes);
    }

    void testRelocatedRows()
    {
        TestDirectory directory{"db-index-update-relocation"};
        const auto root = directory.path / "db";
        auto db = Database::create(root, "test");
        db.executeSql("CREATE TABLE items (id INT, code INT, payload TEXT);");
        db.executeSql("CREATE UNIQUE INDEX by_id ON items (id);");
        db.executeSql("CREATE INDEX by_code ON items (code);");
        db.executeSql("INSERT INTO items VALUES (1, 10, '" + std::string(1600, 'a') +
                      "'), (2, 20, '" + std::string(100, 'b') + "'), (3, 30, '" + std::string(1600, 'c') + "');");
        const auto reused = rowIdFor(root, 1);
        const auto oldSecond = rowIdFor(root, 2);
        db.executeSql("DELETE FROM items WHERE id = 1;");
        db.executeSql("UPDATE items SET payload = 'small' WHERE id = 2;");
        require(rowIdFor(root, 2) == reused && reused != oldSecond, "Fixture did not reuse an earlier table slot");
        verifyIndexes(root, {"by_id", "by_code"});
        const auto oldThird = rowIdFor(root, 3);
        db.executeSql("UPDATE items SET payload = '" + std::string(3000, 'z') + "' WHERE id = 3;");
        require(rowIdFor(root, 3).pageId != oldThird.pageId, "Fixture did not move a row onto a new page");
        verifyIndexes(root, {"by_id", "by_code"});
        const auto before = snapshot(root);
        requireError([&] { db.executeSql("UPDATE items SET payload = '" + std::string(5000, 'x') + "' WHERE id = 2;"); }, "too large");
        require(snapshot(root) == before, "Failed table replacement did not restore removed index entries");
        db.executeSql("DELETE FROM items WHERE id = 3;");
        verifyIndexes(root, {"by_id", "by_code"});
    }

    void testMultiLevelDeletionAndRefill()
    {
        TestDirectory directory{"db-index-delete-tree"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "test");
            db.executeSql("CREATE TABLE items (id INT, bucket INT, text TEXT);");
            std::string insert = "INSERT INTO items VALUES ";
            for (int i = 0; i < 220; ++i)
            {
                if (i) insert += ',';
                insert += '(' + std::to_string(i) + ',' + std::to_string(i % 3) + ", '" +
                          std::string(i % 2 ? 1700 : 900, static_cast<char>('a' + i % 11)) + "')";
            }
            db.executeSql(insert + ';');
            db.executeSql("CREATE INDEX by_key ON items (text, bucket);");
            db.executeSql("CREATE UNIQUE INDEX by_id ON items (id);");
            Tree initial(root, "by_key");
            require(initial.height >= 2, "Fixture did not build a multi-level tree");
            require(db.executeSql("DELETE FROM items WHERE id >= 40 AND id < 180;").affectedRows == 140,
                    "Bulk deletion missed rows");
            verifyIndexes(root, {"by_key", "by_id"});
            db.executeSql("UPDATE items SET text = '" + std::string(1800, 'z') + "', bucket = -1 WHERE id < 20;");
            verifyIndexes(root, {"by_key", "by_id"});
            require(db.executeSql("DELETE FROM items;").affectedRows == 80, "Delete-all missed rows");
            verifyIndexes(root, {"by_key", "by_id"});
        }
        // Reopen an entirely emptied tree, then reuse ranges on both sides of
        // old separators (including keys whose boundary entries were deleted).
        auto db = Database::open(root, "test");
        db.executeSql("INSERT INTO items VALUES (1000, 1, '0'), (1001, 2, 'middle'), (1002, 3, 'zzzz');");
        verifyIndexes(root, {"by_key", "by_id"});
        db.executeSql("UPDATE items SET text = 'same', bucket = 4;");
        verifyIndexes(root, {"by_key", "by_id"});
        db.executeSql("DELETE FROM items WHERE id = 1001;");
        verifyIndexes(root, {"by_key", "by_id"});
    }

    void testExactDeletionAndRollback()
    {
        TestDirectory directory{"db-index-delete-exact"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "test");
            db.executeSql("CREATE TABLE items (id INT, code INT);");
            db.executeSql("INSERT INTO items VALUES (1, 10), (2, 20), (3, 30);");
            db.executeSql("CREATE INDEX by_code ON items (code);");
        }
        const auto rows = readRows(root);
        const auto before = snapshot(root);
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        recovery.begin();
        {
            auto index = Index::open(LinuxDirectory(root / "indexes"), "by_code.index", recovery);
            auto between = rows[1];
            between.row.values[1] = std::int32_t{15};
            auto wrongId = rows[1];
            wrongId.id = RowId{999, 999};
            auto pastEnd = rows[2];
            pastEnd.row.values[1] = std::int32_t{99};
            index.deleteIndexes({between, wrongId, pastEnd});
            require(snapshot(root) == before, "Missing target deleted its lower-bound successor");
            index.deleteIndexes({rows[0], rows[0], rows[1]});
            auto cursor = index.scan();
            const auto survivor = cursor.next();
            require(survivor && survivor->rowId == rows[2].id && !cursor.next(), "Packed deletion removed the wrong entry");
            auto malformed = rows[2];
            malformed.row.values.clear();
            requireError([&] { index.deleteIndexes({rows[2], malformed}); }, "range");
            requireError([&] { recovery.commit(); }, "failed");
        }
        recovery.rollback();
        require(snapshot(root) == before, "Failed batch deletion was not fully rolled back");
        verifyIndexes(root, {"by_code"});
    }

    void testStartupRecovery()
    {
        for (const std::string sql : {"UPDATE items SET code = 40 - code;", "DELETE FROM items WHERE id < 3;"})
        {
            TestDirectory directory{"db-index-mutation-recovery"};
            const auto root = directory.path / "db";
            {
                auto db = Database::create(root, "test");
                db.executeSql("CREATE TABLE items (id INT, code INT);");
                db.executeSql("INSERT INTO items VALUES (1, 10), (2, 20), (3, 30);");
                db.executeSql("CREATE UNIQUE INDEX by_code ON items (code);");
                db.executeSql("CREATE INDEX by_id ON items (id);");
            }
            const auto before = snapshot(root);
            {
                auto engine = StorageEngine::open(root);
                Lexer lexer{sql};
                Parser parser{lexer.tokenize()};
                QueryValidator binder{engine};
                const auto bound = binder.validate(parser.parseStatement());
                engine.beginStatement();
                if (const auto *update = std::get_if<BoundUpdate>(&bound)) engine.updateRows(*update);
                else engine.deleteRows(std::get<BoundDelete>(bound));
                require(snapshot(root) != before, "Uncommitted mutation did not write the fixture");
            }
            { auto engine = StorageEngine::open(root); }
            require(snapshot(root) == before, "Startup recovery did not undo table and index mutations");
            verifyIndexes(root, {"by_code", "by_id"});
        }
    }
}

int main()
{
    try
    {
        testUniqueUpdatesAndDeletes();
        testRelocatedRows();
        testMultiLevelDeletionAndRefill();
        testExactDeletionAndRollback();
        testStartupRecovery();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
