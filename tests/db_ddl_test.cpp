#include "db_database.h"
#include "db_query_exception.h"
#include "db_test_directory.h"
#include "db_test_journal.h"
#include "db_write.h"
#include "db_page_factory.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>

namespace
{
    using Path = std::filesystem::path;
    using Image = std::map<Path, std::vector<std::byte>>;

    void require(bool condition, const std::string &message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    Image files(const Path &root)
    {
        Image image;
        for (const auto &directory : {"tables", "indexes"})
            for (const auto &file : std::filesystem::directory_iterator(root / directory))
                image.emplace(file.path().lexically_relative(root), journal_test::readFile(file.path()));
        return image;
    }

    void reject(Database &db, const std::string &sql, QueryErrorCode code)
    {
        const auto result = db.tryExecuteSql(sql);
        require(!result && result.error().code == code, "Expected query rejection: " + sql);
    }

    void seed(const Path &root)
    {
        auto db = Database::create(root, "test");
        db.executeSql("CREATE TABLE parents (id INT PRIMARY KEY);");
        db.executeSql("CREATE TABLE others (id INT PRIMARY KEY);");
        db.executeSql("CREATE TABLE children (id INT PRIMARY KEY, parent_id INT REFERENCES parents(id), "
                      "second_parent INT REFERENCES parents(id), other_id INT REFERENCES others(id), code INT, "
                      "label TEXT DEFAULT 'kept', CHECK (code > 0));");
        db.executeSql("CREATE TABLE grandchildren (id INT REFERENCES children(id));");
        db.executeSql("CREATE TABLE independent (id INT);");
        db.executeSql("CREATE INDEX parent_index ON parents(id);");
        db.executeSql("CREATE UNIQUE INDEX child_code ON children(code);");
        db.executeSql("CREATE INDEX grandchild_index ON grandchildren(id);");
        db.executeSql("INSERT INTO parents VALUES (1);");
        db.executeSql("INSERT INTO others VALUES (1);");
        db.executeSql("INSERT INTO independent VALUES (1);");
        for (int id = 1; id <= 12; ++id)
        {
            db.executeSql("INSERT INTO children VALUES (" + std::to_string(id) +
                          ", 1, 1, 1, " + std::to_string(id) + ", '" + std::string(3000, 'x') + "');");
            db.executeSql("INSERT INTO grandchildren VALUES (" + std::to_string(id) + ");");
        }
    }

    void testSqlAndNames()
    {
        TestDirectory directory{"db-ddl-names"};
        const auto root = directory.path / "db";
        auto db = Database::create(root, "test");
        db.executeSql("CREATE TABLE a (id INT);");
        db.executeSql("CREATE TABLE b (id INT);");
        db.executeSql("CREATE INDEX by_a ON a(id);");
        db.executeSql("CREATE INDEX a ON b(id);");
        db.executeSql("INSERT INTO a VALUES (1);");
        db.executeSql("INSERT INTO b VALUES (2);");
        const auto before = files(root);
        for (const auto &sql : {"DROP TABLE a,;", "TRUNCATE TABLE a,;",
                 "DROP TABLE a nonsense;", "TRUNCATE TABLE a nonsense;", "DROP INDEX by_a nonsense;"})
            reject(db, sql, QueryErrorCode::Syntax);
        reject(db, "DROP TABLE a, missing;", QueryErrorCode::Validation);
        reject(db, "TRUNCATE TABLE a, missing;", QueryErrorCode::Validation);
        reject(db, "DROP INDEX missing;", QueryErrorCode::Validation);
        require(files(root) == before, "Rejected DDL modified storage");
        db.executeSql("TRUNCATE TABLE a, b, a;");
        require(db.executeSql("SELECT * FROM a;").rows.empty() &&
                db.executeSql("SELECT * FROM b;").rows.empty(), "Multiple-table TRUNCATE failed");
        db.executeSql("DROP TABLE a;");
        require(!std::filesystem::exists(root / "tables/a.table") &&
                !std::filesystem::exists(root / "indexes/by_a.index") &&
                std::filesystem::exists(root / "indexes/a.index"), "DROP used table names as index names");
        db.executeSql("DROP INDEX a;");
        require(std::filesystem::exists(root / "tables/b.table") &&
                !std::filesystem::exists(root / "indexes/a.index"), "DROP INDEX deleted the wrong object");
        db.executeSql("DROP TABLE b;");
    }

    void testDropCascade()
    {
        TestDirectory directory{"db-ddl-drop-cascade"};
        const auto root = directory.path / "db";
        seed(root);
        {
            auto db = Database::open(root, "test");
            const auto before = files(root);
            reject(db, "DROP TABLE independent, parents;", QueryErrorCode::Constraint);
            require(files(root) == before, "DROP modified an earlier target before checking FKs");
            db.executeSql("DROP TABLE parents CASCADE;");
            require(db.executeSql("SELECT * FROM children;").rows.size() == 12 &&
                    db.executeSql("SELECT * FROM grandchildren;").rows.size() == 12,
                    "DROP CASCADE deleted referencing tables or data");
            require(journal_test::readFile(root / "indexes/child_code.index") ==
                    before.at("indexes/child_code.index"), "DROP CASCADE changed the surviving index");
        }
        {
            auto db = Database::open(root, "test");
            db.executeSql("INSERT INTO children(id, parent_id, second_parent, other_id, code) "
                          "VALUES (20, 999, 999, 1, 20);");
            reject(db, "INSERT INTO children(id, other_id, code) VALUES (21, 999, 21);",
                   QueryErrorCode::Constraint);
            reject(db, "INSERT INTO children(id, code) VALUES (21, -1);", QueryErrorCode::Constraint);
            require(std::get<std::string>(db.executeSql("SELECT label FROM children WHERE id = 20;").rows[0].values[0])
                    == "kept", "DEFAULT was removed along with the FK");
            db.executeSql("DROP TABLE children, grandchildren;");
        }
        require(!std::filesystem::exists(root / "indexes/child_code.index") &&
                !std::filesystem::exists(root / "indexes/grandchild_index.index"), "DROP left orphan indexes");
    }

    void testTruncateCascade()
    {
        TestDirectory directory{"db-ddl-truncate"};
        const auto root = directory.path / "db";
        seed(root);
        {
            auto db = Database::open(root, "test");
            const auto before = files(root);
            reject(db, "TRUNCATE TABLE independent, parents;", QueryErrorCode::Constraint);
            require(files(root) == before, "Rejected TRUNCATE changed an earlier target");
            db.executeSql("TRUNCATE TABLE parents CASCADE;");
            for (const auto &name : {"parents", "children", "grandchildren"})
            {
                require(db.executeSql("SELECT * FROM " + std::string(name) + ";").rows.empty(),
                        "TRUNCATE missed a transitive reference");
                require(std::filesystem::file_size(root / "tables" / (std::string(name) + ".table")) == 2 * PAGE_SIZE,
                        "TRUNCATE retained data pages");
            }
            for (const auto &name : {"parent_index", "child_code", "grandchild_index"})
                require(std::filesystem::exists(root / "indexes" / (std::string(name) + ".index")),
                        "TRUNCATE deleted an index definition");
        }
        {
            auto db = Database::open(root, "test");
            db.executeSql("INSERT INTO parents VALUES (1);");
            db.executeSql("INSERT INTO children(id, parent_id, other_id, code) VALUES (1, 1, 1, 1);");
            reject(db, "INSERT INTO children(id, parent_id, code) VALUES (2, 1, 1);", QueryErrorCode::Constraint);
            reject(db, "INSERT INTO children(id, parent_id, code) VALUES (2, 99, 2);", QueryErrorCode::Constraint);
            db.executeSql("INSERT INTO grandchildren VALUES (1);");
            db.executeSql("TRUNCATE TABLE parents, children, grandchildren;");
            require(db.executeSql("SELECT * FROM independent;").rows.size() == 1,
                    "TRUNCATE changed an unrelated table");
        }
    }

    enum class Operation { DropTable, DropIndex, Truncate };

    void mutate(StorageEngine &engine, Operation op)
    {
        switch (op)
        {
        case Operation::DropTable: engine.dropTable({{"parents"}, true}); break;
        case Operation::DropIndex: engine.dropIndex({"child_code"}); break;
        case Operation::Truncate: engine.truncateTable({{"parents"}, true}); break;
        }
    }

    void testRecovery()
    {
        for (const auto op : {Operation::DropTable, Operation::DropIndex, Operation::Truncate})
        {
            for (const bool reopen : {false, true})
            {
                TestDirectory directory{"db-ddl-recovery"};
                const auto root = directory.path / "db";
                seed(root);
                const auto before = files(root);
                {
                    auto engine = StorageEngine::open(root);
                    engine.beginStatement();
                    mutate(engine, op);
                    require(files(root) != before, "Recovery fixture made no changes");
                    if (!reopen) engine.rollbackStatement();
                    // Otherwise leave a durable journal for startup recovery.
                }
                {
                    auto db = Database::open(root, "test");
                    require(files(root) == before, "DDL rollback did not restore exact table/index bytes");
                    require(db.executeSql("SELECT * FROM children;").rows.size() == 12,
                            "DDL rollback lost rows");
                }
                require(!std::filesystem::exists(root / "journal/journal.log"), "Recovery retained its journal");
            }
        }
    }

    void testCyclesAndHandles()
    {
        TestDirectory directory{"db-ddl-cycles"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "test");
            db.executeSql("CREATE TABLE a (id INT PRIMARY KEY);");
            db.executeSql("CREATE TABLE b (id INT PRIMARY KEY REFERENCES a(id));");
            db.executeSql("INSERT INTO a VALUES (1);");
            db.executeSql("INSERT INTO b VALUES (1);");
        }
        // Seed a cycle and self-reference; CREATE cannot bind forward references.
        {
            PageFile file{LinuxDirectory{root / "tables"}, "a.table"};
            auto page = decodeHeaderPage(file.readPage(HeaderPageId));
            auto &header = std::get<HeaderPage>(page.data);
            header.constraints.push_back(BoundForeignKeyConstraintExpr{"a_b", {0}, "b", {0}});
            header.constraints.push_back(BoundForeignKeyConstraintExpr{"a_a", {0}, "a", {0}});
            file.writePage(HeaderPageId, encodeHeaderPage(page.header, header));
            file.sync();
        }
        {
            auto engine = StorageEngine::open(root);
            auto oldHandle = engine.openTable("a");
            engine.beginStatement();
            engine.truncateTable({{"a"}, true});
            engine.commitStatement();
            bool rejected = false;
            try { (void)oldHandle.scan(); }
            catch (const std::runtime_error &) { rejected = true; }
            require(rejected, "TRUNCATE allowed use of a stale table handle");
        }
        {
            auto db = Database::open(root, "test");
            require(db.executeSql("SELECT * FROM a;").rows.empty() && db.executeSql("SELECT * FROM b;").rows.empty(),
                    "TRUNCATE failed to resolve an FK cycle");
            db.executeSql("DROP TABLE a, b;");
        }
    }

    HeaderPage readSchema(const Path &root, const std::string &table)
    {
        return std::get<HeaderPage>(decodeHeaderPage(readPageFromFile(
            LinuxDirectory{root / "tables"}, table + ".table", HeaderPageId)).data);
    }

    const BoundConstraint *findConstraint(const HeaderPage &schema, const std::string &name)
    {
        const auto it = std::ranges::find_if(schema.constraints, [&](const BoundConstraint &constraint)
                                            {
                                                return std::visit([&](const BoundConstraintExpr &value)
                                                                  { return value.constraintName == name; }, constraint);
                                            });
        return it == schema.constraints.end() ? nullptr : &*it;
    }

    void testDropColumnLocalConstraints()
    {
        TestDirectory directory{"db-ddl-drop-column-local"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "test");
            db.executeSql("CREATE TABLE items (removed INT CONSTRAINT removed_null NULL "
                          "CONSTRAINT removed_default DEFAULT 1, "
                          "kept INT CONSTRAINT kept_nn NOT NULL CONSTRAINT kept_default DEFAULT 7, label TEXT, "
                          "CONSTRAINT removed_check CHECK (removed > 0), "
                          "CONSTRAINT kept_check CHECK (-kept < 0 AND label IS NOT NULL), "
                          "CONSTRAINT removed_unique UNIQUE (kept, removed), "
                          "CONSTRAINT kept_unique UNIQUE (label, kept));");
            db.executeSql("CREATE INDEX by_removed ON items(removed);");
            db.executeSql("CREATE INDEX by_label ON items(label);");
            db.executeSql("INSERT INTO items VALUES (1, 2, 'first'), (2, 3, 'second');");
            db.executeSql("ALTER TABLE items DROP COLUMN removed;");
            const auto schema = readSchema(root, "items");
            for (const std::string name : {"removed_null", "removed_default", "removed_check", "removed_unique"})
                require(!findConstraint(schema, name), "Retained a constraint on the dropped column: " + name);
            require(std::get<BoundNotNullConstraintExpr>(*findConstraint(schema, "kept_nn")).columnId == 0 &&
                        std::get<BoundDefaultConstraintExpr>(*findConstraint(schema, "kept_default")).columnId == 0 &&
                        std::get<BoundUniqueConstraintExpr>(*findConstraint(schema, "kept_unique")).columnIds ==
                            std::vector<ColumnId>{1, 0}, "Surviving constraint IDs were not remapped");
            require(!std::filesystem::exists(root / "indexes/by_removed.index"), "Kept an index on the removed column");
            db.executeSql("INSERT INTO items (label) VALUES ('defaulted');");
            reject(db, "INSERT INTO items VALUES (0, 'bad');", QueryErrorCode::Constraint);
            reject(db, "INSERT INTO items VALUES (3, NULL);", QueryErrorCode::Constraint);
            reject(db, "INSERT INTO items VALUES (2, 'first');", QueryErrorCode::Constraint);
            require(db.executeSql("SELECT * FROM items;").rows.size() == 3, "DROP COLUMN lost rows");
        }
        auto engine = StorageEngine::open(root);
        require(engine.getIndexHeader("by_label").keyColumns == std::vector<ColumnId>{1},
                "Surviving index retained the old table column ID");
        for (auto &index : engine.indexesForTable("items"))
        {
            auto cursor = index.scan();
            std::size_t count = 0;
            while (cursor.next()) ++count;
            require(count == 3, "An index was not rebuilt after dropping a column");
        }
    }

    void seedColumnReferences(const Path &root)
    {
        auto db = Database::create(root, "test");
        db.executeSql("CREATE TABLE parents (padding INT, id INT PRIMARY KEY, code INT UNIQUE);");
        db.executeSql("CREATE TABLE other_parents (id INT PRIMARY KEY);");
        db.executeSql("CREATE TABLE children (padding INT, parent_id INT, code INT, other_id INT, "
                      "CONSTRAINT fk_id FOREIGN KEY (parent_id) REFERENCES parents(id), "
                      "CONSTRAINT fk_code FOREIGN KEY (code) REFERENCES parents(code), "
                      "CONSTRAINT fk_other FOREIGN KEY (other_id) REFERENCES other_parents(id));");
        db.executeSql("INSERT INTO parents VALUES (0, 1, 10);");
        db.executeSql("INSERT INTO other_parents VALUES (5);");
        db.executeSql("INSERT INTO children VALUES (0, 1, 10, 5);");
    }

    void testDropColumnForeignKeys()
    {
        TestDirectory directory{"db-ddl-drop-column-fk"};
        const auto root = directory.path / "db";
        seedColumnReferences(root);
        {
            auto db = Database::open(root, "test");
            // Dropping an unrelated column still moves referenced IDs.
            db.executeSql("ALTER TABLE parents DROP COLUMN padding;");
            auto schema = readSchema(root, "children");
            const auto &idFk = std::get<BoundForeignKeyConstraintExpr>(*findConstraint(schema, "fk_id"));
            const auto &codeFk = std::get<BoundForeignKeyConstraintExpr>(*findConstraint(schema, "fk_code"));
            require(idFk.localColumnIds == std::vector<ColumnId>{1} && idFk.referencedColumnIds == std::vector<ColumnId>{0} &&
                        codeFk.localColumnIds == std::vector<ColumnId>{2} && codeFk.referencedColumnIds == std::vector<ColumnId>{1},
                    "Incoming FKs did not remap referenced IDs independently of local IDs");
            // Changing the child only moves local IDs, even when numeric IDs overlap.
            db.executeSql("ALTER TABLE children DROP COLUMN padding;");
            db.executeSql("INSERT INTO children VALUES (1, 10, 5);");
            reject(db, "INSERT INTO children VALUES (9, 10, 5);", QueryErrorCode::Constraint);

            const auto before = files(root);
            reject(db, "ALTER TABLE parents DROP COLUMN id;", QueryErrorCode::Constraint);
            require(files(root) == before, "Rejected DROP COLUMN changed files");

            const auto childrenBefore = db.executeSql("SELECT * FROM children;").rows;
            db.executeSql("ALTER TABLE parents DROP COLUMN id CASCADE;");
            schema = readSchema(root, "children");
            require(!findConstraint(schema, "fk_id") && findConstraint(schema, "fk_code") && findConstraint(schema, "fk_other"),
                    "CASCADE removed the wrong foreign keys");
            const auto &remaining = std::get<BoundForeignKeyConstraintExpr>(*findConstraint(schema, "fk_code"));
            require(remaining.localColumnIds == std::vector<ColumnId>{1} && remaining.referencedColumnIds == std::vector<ColumnId>{0},
                    "CASCADE did not remap the surviving referenced key");
            const auto childrenAfter = db.executeSql("SELECT * FROM children;").rows;
            require(childrenAfter.size() == childrenBefore.size(), "CASCADE deleted referencing rows");
            for (std::size_t i = 0; i < childrenBefore.size(); ++i)
                require(childrenBefore[i].values == childrenAfter[i].values, "CASCADE changed referencing row values");
            db.executeSql("INSERT INTO children VALUES (999, 10, 5);");
            reject(db, "INSERT INTO children VALUES (999, 99, 5);", QueryErrorCode::Constraint);
            reject(db, "INSERT INTO children VALUES (999, 10, 99);", QueryErrorCode::Constraint);
        }
        auto reopened = Database::open(root, "test");
        reopened.executeSql("INSERT INTO children VALUES (999, 10, 5);");
        reject(reopened, "INSERT INTO children VALUES (999, 99, 5);", QueryErrorCode::Constraint);
    }

    void testDropColumnSelfReferences()
    {
        TestDirectory directory{"db-ddl-drop-column-self"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "test");
            db.executeSql("CREATE TABLE nodes (padding INT, id INT PRIMARY KEY, parent_id INT);");
            db.executeSql("INSERT INTO nodes VALUES (0, 1, 1);");
        }
        {
            PageFile file{LinuxDirectory{root / "tables"}, "nodes.table"};
            auto page = decodeHeaderPage(file.readPage(HeaderPageId));
            auto &schema = std::get<HeaderPage>(page.data);
            schema.constraints.push_back(BoundForeignKeyConstraintExpr{"self_fk", {2}, "nodes", {1}});
            page.header.freeSpaceStart = calculateHeaderFreeSpaceStart(page.header, schema);
            file.writePage(HeaderPageId, encodeHeaderPage(page.header, schema));
            file.sync();
        }
        auto db = Database::open(root, "test");
        db.executeSql("ALTER TABLE nodes DROP COLUMN padding;");
        const auto schema = readSchema(root, "nodes");
        const auto &fk = std::get<BoundForeignKeyConstraintExpr>(*findConstraint(schema, "self_fk"));
        require(fk.localColumnIds == std::vector<ColumnId>{1} && fk.referencedColumnIds == std::vector<ColumnId>{0},
                "Self-reference did not remap both sides");
        db.executeSql("INSERT INTO nodes VALUES (2, 1);");
        reject(db, "INSERT INTO nodes VALUES (3, 99);", QueryErrorCode::Constraint);
        // A self-reference is local and does not require CASCADE.
        db.executeSql("ALTER TABLE nodes DROP COLUMN id;");
        require(!findConstraint(readSchema(root, "nodes"), "self_fk"), "Kept a self-FK referencing the dropped column");
        db.executeSql("INSERT INTO nodes VALUES (99);");
    }

    void testDropColumnRecovery()
    {
        for (const bool reopen : {false, true})
        {
            TestDirectory directory{"db-ddl-drop-column-recovery"};
            const auto root = directory.path / "db";
            seedColumnReferences(root);
            const auto before = files(root);
            {
                auto engine = StorageEngine::open(root);
                const auto schema = engine.getTableHeader("parents");
                engine.beginStatement();
                engine.alterTable(BoundAlterTable{"parents", BoundDropColumnAction{schema.columns.at(1), true}});
                require(files(root) != before, "DROP COLUMN recovery fixture made no changes");
                if (!reopen) engine.rollbackStatement();
            }
            auto db = Database::open(root, "test");
            require(files(root) == before, "DROP COLUMN recovery did not restore exact table/index bytes");
            reject(db, "INSERT INTO children VALUES (0, 99, 10, 5);", QueryErrorCode::Constraint);
        }
    }
}

int main()
{
    try
    {
        testSqlAndNames();
        testDropCascade();
        testTruncateCascade();
        testRecovery();
        testCyclesAndHandles();
        testDropColumnLocalConstraints();
        testDropColumnForeignKeys();
        testDropColumnSelfReferences();
        testDropColumnRecovery();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
