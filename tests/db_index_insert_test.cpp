#include "db_database.h"
#include "db_page_factory.h"
#include "db_test_directory.h"
#include "db_test_index.h"
#include "db_test_journal.h"
#include "db_write.h"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>

namespace
{
    using index_test::require;
    using index_test::same;
    using index_test::Tree;

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

    CreateIndexPayload payload(const std::string &name, bool unique = false)
    {
        Column text{};
        text.name = "text";
        text.type = DataType::Text;
        text.nullable = true;
        Column number{};
        number.name = "number";
        number.type = DataType::Int;
        number.nullable = false;
        return {.indexName = name, .tableName = "items", .magic = "MYIX",
                .keyColumns = {2, 0}, .columns = buildStoredColumns({text, number}), .unique = unique};
    }

    LeafIndexEntry generated(std::size_t i)
    {
        const auto group = i % 29;
        const auto length = group % 3 == 0 ? 1900u : group % 3 == 1 ? 650u : 5u;
        Value text = std::string(length, static_cast<char>('a' + group % 26));
        if (group == 0) text = std::monostate{};
        return {Row{{text, static_cast<std::int32_t>(i % 5)}},
                RowId{static_cast<PageId>(i / 9 + 1), static_cast<std::uint16_t>(i % 9)}};
    }

    void verifyScan(Index &index, const std::vector<LeafIndexEntry> &expected)
    {
        auto cursor = index.scan();
        for (const auto &entry : expected)
        {
            auto actual = cursor.next();
            require(actual && same(*actual, entry), "Scan returned incorrect key or table RowId");
        }
        require(!cursor.next(), "Scan returned extra entries");
    }

    void verifySeeks(Index &index, const std::vector<LeafIndexEntry> &expected)
    {
        auto cursor = index.scan();
        for (const auto &entry : expected)
        {
            cursor.seek(entry.key, entry.rowId);
            const auto exact = cursor.next();
            require(exact && same(*exact, entry), "Full tuple seek routed incorrectly");
            Row prefix{{entry.key.values.front()}};
            cursor.seek(prefix);
            const auto first = cursor.next();
            const auto reference = std::find_if(expected.begin(), expected.end(), [&](const auto &candidate) {
                return candidate.key.values.front() == prefix.values.front();
            });
            require(first && same(*first, *reference), "Prefix seek skipped a matching entry");
        }
        cursor.seek(Row{});
        require(same(*cursor.next(), expected.front()), "Empty prefix did not rewind");
        cursor.seek(Row{{std::string(1950, 'z'), std::int32_t{999}}});
        require(!cursor.next(), "Seek past the last key returned an entry");
        cursor.seek(expected.back().key, expected.back().rowId);
        auto moved = std::move(cursor);
        require(!cursor.next() && same(*moved.next(), expected.back()), "Moved cursor lost its position");
    }

    void testSplitOrders()
    {
        for (const std::string order : {"ascending", "descending", "random"})
        {
            TestDirectory directory{"db-index-insert-" + order};
            const auto root = directory.path / "db";
            { auto db = Database::create(root, "test"); }
            const journal_test::DatabaseIdentity identity{root};
            StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
            std::vector<LeafIndexEntry> expected;
            for (std::size_t i = 0; i < 440; ++i) expected.push_back(generated(i));
            std::sort(expected.begin(), expected.end(), indexEntryLess);
            auto input = expected;
            if (order == "descending") std::reverse(input.begin(), input.end());
            if (order == "random")
            {
                std::mt19937 random{42};
                std::shuffle(input.begin(), input.end(), random);
            }
            recovery.begin();
            {
                auto index = Index::create(LinuxDirectory(root / "indexes"), payload("test"), recovery);
                index.insertIndexes(input);
                verifyScan(index, expected);
                verifySeeks(index, expected);
            }
            recovery.commit();
            Tree tree(root, "test");
            require(tree.height >= 2 && tree.leaves.size() > 64, "Fixture did not force internal splits");
            tree.verifyRouting();
            require(tree.entries.size() == expected.size(), "Split lost or duplicated entries");
            for (std::size_t i = 0; i < expected.size(); ++i)
                require(same(tree.entries[i], expected[i]), "Stored ordering differs from sorted input");
            auto reopened = Index::open(LinuxDirectory(root / "indexes"), "test.index", recovery);
            verifyScan(reopened, expected);
            verifySeeks(reopened, expected);
            // A subsequent transaction must capture fresh before-images for this handle.
            const auto before = journal_test::readFile(root / "indexes/test.index");
            recovery.begin();
            reopened.insertIndexes(std::vector<LeafIndexEntry>{generated(1000)});
            recovery.rollback();
            require(journal_test::readFile(root / "indexes/test.index") == before, "Reused handle rollback failed");
        }
    }

    void testExactFitAndBulkAppend()
    {
        TestDirectory directory{"db-index-insert-fit"};
        const auto root = directory.path / "db";
        { auto db = Database::create(root, "test"); }
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        const RowSchema schema{payload("test").columns};
        LeafIndexEntry entry{Row{{std::string{}, std::int32_t{1}}}, RowId{1, 0}};
        constexpr auto payloadBytes = PAGE_SIZE - PageHeaderLayout::Size - IndexPageHeader::serializedSize();
        const auto textLength = payloadBytes / 4 - entry.serializedSize(schema) - encodedSlotSize();
        entry.key.values[0] = std::string(textLength, 'a');
        std::vector<LeafIndexEntry> entries;
        for (std::uint16_t i = 0; i < 4; ++i)
        {
            entry.rowId.slotIndex = i;
            entries.push_back(entry);
        }
        recovery.begin();
        {
            auto index = Index::create(LinuxDirectory(root / "indexes"), payload("test"), recovery);
            index.insertIndexes(entries);
        }
        recovery.commit();
        Tree full(root, "test");
        require(full.height == 0 && full.entries.size() == 4, "Exact fit incorrectly split the leaf");
        const auto page = decodeIndexPage(full.file.readPage(full.metadata.rootPageId), schema);
        require(page.header.freeSpaceStart == page.header.freeSpaceEnd, "Exact fit fixture has free space");
        recovery.begin();
        {
            auto index = Index::open(LinuxDirectory(root / "indexes"), "test.index", recovery);
            entry.rowId.slotIndex = 4;
            index.insertIndexes(std::vector<LeafIndexEntry>{entry});
        }
        recovery.commit();
        Tree split(root, "test");
        require(split.height == 1 && split.entries.size() == 5, "Full leaf did not split");

        recovery.begin();
        {
            auto index = Index::create(LinuxDirectory(root / "indexes"), payload("bulk"), recovery);
            std::size_t i = 0;
            index.bulkLoad([&]() -> std::optional<LeafIndexEntry> {
                if (i == 120) return std::nullopt;
                return generated(i++);
            });
        }
        recovery.commit();
        recovery.begin();
        std::vector<LeafIndexEntry> expected;
        for (std::size_t i = 0; i < 240; ++i) expected.push_back(generated(i));
        {
            auto index = Index::open(LinuxDirectory(root / "indexes"), "bulk.index", recovery);
            index.insertIndexes(std::vector<LeafIndexEntry>(expected.begin() + 120, expected.end()));
            std::sort(expected.begin(), expected.end(), indexEntryLess);
            verifyScan(index, expected);
        }
        recovery.commit();
        Tree bulk(root, "bulk");
        bulk.verifyRouting();
        require(bulk.entries.size() == expected.size(), "Incremental insert into bulk tree lost entries");
    }

    void testFailureAndRecovery()
    {
        TestDirectory directory{"db-index-insert-recovery"};
        const auto root = directory.path / "db";
        { auto db = Database::create(root, "test"); }
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        recovery.begin();
        {
            auto index = Index::create(LinuxDirectory(root / "indexes"), payload("test"), recovery);
            index.insertIndexes(std::vector<LeafIndexEntry>{generated(0)});
        }
        recovery.commit();
        const auto before = journal_test::readFile(root / "indexes/test.index");
        for (const std::string failure : {"duplicate", "oversize", "type", "count"})
        {
            recovery.begin();
            {
                auto index = Index::open(LinuxDirectory(root / "indexes"), "test.index", recovery);
                std::vector<LeafIndexEntry> batch;
                for (std::size_t i = 1; i < 100; ++i) batch.push_back(generated(i));
                // Persist splits first, then fail after another successful in-memory insertion.
                index.insertIndexes(batch);
                auto bad = generated(0);
                std::string message = "already exists";
                if (failure == "oversize") { bad.key.values[0] = std::string(3000, 'x'); message = "too large"; }
                if (failure == "type") { bad.key.values[1] = std::string("wrong"); message = "expects type"; }
                if (failure == "count") { bad.key.values.pop_back(); message = "schema expects"; }
                requireError([&] { index.insertIndexes(std::vector<LeafIndexEntry>{generated(101), bad}); }, message);
                requireError([&] { recovery.commit(); }, "failed");
            }
            recovery.rollback();
            require(journal_test::readFile(root / "indexes/test.index") == before, "Failed insertion was not fully rolled back");
        }
        // A completed write without statement commit must be undone on startup.
        recovery.begin();
        {
            auto index = Index::open(LinuxDirectory(root / "indexes"), "test.index", recovery);
            std::vector<LeafIndexEntry> batch;
            for (std::size_t i = 1; i < 100; ++i) batch.push_back(generated(i));
            index.insertIndexes(batch);
        }
        { auto engine = StorageEngine::open(root); }
        require(journal_test::readFile(root / "indexes/test.index") == before, "Startup recovery did not undo splits");
    }

    void testSqlUniqueAndProjection()
    {
        TestDirectory directory{"db-index-insert-sql"};
        const auto root = directory.path / "db";
        auto db = Database::create(root, "test");
        db.executeSql("CREATE TABLE items (number INT, unused TEXT, text TEXT);");
        db.executeSql("CREATE UNIQUE INDEX unique_key ON items (text, number);");
        db.executeSql("CREATE INDEX reversed ON items (number, text);");
        db.executeSql("INSERT INTO items VALUES (2, 'ignored', 'b'), (1, 'ignored', 'a'), (1, 'ignored', NULL), (1, 'ignored', NULL);");
        Tree unique(root, "unique_key");
        Tree reversed(root, "reversed");
        require(unique.entries.size() == 4 && reversed.entries.size() == 4, "SQL insert did not maintain both indexes");
        require(unique.metadata.keyColumns == std::vector<ColumnId>{2, 0} &&
                reversed.metadata.keyColumns == std::vector<ColumnId>{0, 2}, "Wrong key column order");
        require(unique.entries[2].key.values == Row{{std::string("a"), std::int32_t{1}}}.values,
                "LocatedRow projection did not follow index-column order");
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        auto table = Table::open(LinuxDirectory(root / "tables"), "items.table", recovery);
        auto scan = table.scan();
        while (const auto row = scan.nextLocated())
        {
            require(std::ranges::any_of(unique.entries, [&](const auto &entry) {
                return entry.rowId == row->id && entry.key.values == Row{{row->row.values[2], row->row.values[0]}}.values;
            }), "Index does not contain the actual table RowId");
        }
        const auto tableBefore = journal_test::readFile(root / "tables/items.table");
        const auto uniqueBefore = journal_test::readFile(root / "indexes/unique_key.index");
        const auto reversedBefore = journal_test::readFile(root / "indexes/reversed.index");
        requireError([&] {
            db.executeSql("INSERT INTO items VALUES (3, 'ignored', 'c'), (1, 'ignored', 'a');");
        }, "UNIQUE INDEX");
        requireError([&] {
            db.executeSql("INSERT INTO items VALUES (9, 'ignored', 'same'), (9, 'ignored', 'same');");
        }, "UNIQUE INDEX");
        require(journal_test::readFile(root / "tables/items.table") == tableBefore, "Unique failure left table rows");
        require(journal_test::readFile(root / "indexes/unique_key.index") == uniqueBefore, "Unique failure changed index");
        require(journal_test::readFile(root / "indexes/reversed.index") == reversedBefore, "Unique failure changed other index");
        db.executeSql("INSERT INTO items VALUES (3, 'ignored', 'c');");
        Tree after(root, "unique_key");
        require(after.entries.size() == 5, "Insert after rollback failed");
    }
}

int main()
{
    try
    {
        testSplitOrders();
        testExactFitAndBulkAppend();
        testFailureAndRecovery();
        testSqlUniqueAndProjection();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
