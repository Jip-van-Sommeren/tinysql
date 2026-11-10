#include "db_test_journal.h"
#include "db_buffer_manager.h"
#include "db_database.h"
#include "db_page_factory.h"
#include "db_table.h"
#include "db_read.h"
#include "db_write.h"
#include "db_test_directory.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    void require(bool condition, const std::string &message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename Function>
    void requireThrows(Function function, const std::string &expected)
    {
        try
        {
            function();
        }
        catch (const std::exception &error)
        {
            require(std::string{error.what()}.find(expected) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected error containing: " + expected);
    }

    std::int32_t nextId(TableCursor &cursor)
    {
        const auto row = cursor.next();
        require(row.has_value(), "Cursor ended before the expected row");
        return std::get<std::int32_t>(row->values.at(0));
    }

    std::vector<std::int32_t> scanIds(Table &table)
    {
        auto cursor = table.scan();
        std::vector<std::int32_t> ids;
        while (auto row = cursor.next())
        {
            ids.push_back(std::get<std::int32_t>(row->values.at(0)));
        }
        require(!cursor.next(), "An exhausted cursor returned another row");
        return ids;
    }

    void deleteId(Table &table, StatementRecovery &recovery, std::int32_t id)
    {
        std::vector<LocatedRow> rows;
        {
            auto cursor = table.scan();
            while (auto row = cursor.nextLocated())
            {
                if (std::get<std::int32_t>(row->row.values.at(0)) == id)
                    rows.push_back(std::move(*row));
            }
        }
        recovery.begin();
        require(table.deleteRows(rows) == 1, "DELETE did not remove the expected row");
        recovery.commit();
    }

    void testPageGuards(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE guard_values (id INT);");
        database.executeSql("INSERT INTO guard_values VALUES (1);");
        const std::filesystem::path relativeFile = "tables/guard_values.table";
        const auto file = path / relativeFile;
        const Page headerSnapshot = decodeHeaderPage(readPageFromFile(LinuxDirectory{file.parent_path()}, file.filename(), 0));
        const HeaderPage &schema = std::get<HeaderPage>(headerSnapshot.data);
        // Seed extra physical pages before opening the cache, instead of using
        // the now-private setPage() as a fixture-building API.
        {
            PageFile pages{LinuxDirectory{file.parent_path()}, file.filename()};
            for (PageId id = 2; id < 128; ++id)
            {
                const Page page = makeEmptyDataPage(id);
                pages.writePage(id, encodeDataPage(page.header, schema, std::get<DataPage>(page.data)));
            }
            pages.sync();
        }
        const journal_test::DatabaseIdentity identity{path};
        StatementRecovery recovery{LinuxDirectory{path}, identity.bytes};
        BufferManager manager{LinuxDirectory{path / "tables"}, relativeFile.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
        {
            auto guard = manager.getPage<DataPage>(1);
            requireThrows([&]
                          { guard.write<DataPage>(); }, "active statement");
        }
        recovery.begin();
        {
            auto first = manager.getPage<DataPage>(1);
            {
                const auto second = manager.getPage<DataPage>(1);
                require(&first.page() == &second.page(),
                        "Guards do not reference the same cached page");
                first.write<DataPage>().rows.at(0).row.values.at(0) = std::int32_t{42};
                require(std::get<std::int32_t>(second.as<DataPage>().rows.at(0).row.values.at(0)) == 42,
                        "Guard writes were not visible through another guard");
            }

            // Grow the map while keeping a page pinned: rehash must not move it.
            const Page *address = &first.page();
            for (PageId id = 2; id < 128; ++id)
            {
                manager.getPage<DataPage>(id);
            }
            const auto again = manager.getPage<DataPage>(1);
            require(&again.page() == address, "Cache growth invalidated a guard");
            manager.flushPage(1);
            manager.sync();
        }
        recovery.commit();

        BufferManager reopened{LinuxDirectory{path / "tables"}, relativeFile.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
        require(std::get<std::int32_t>(reopened.getPage<DataPage>(1).as<DataPage>().rows.at(0).row.values.at(0)) == 42,
                "Dirty page changes did not survive reopening");

        // Initialization replaces cached page zero through the public API. It
        // must reject a pinned header and succeed once its last guard releases.
        recovery.begin();
        BufferManager pins{LinuxDirectory{path / "tables"}, "pins.table", LinuxFile::OpenMode::CreateNew, recovery};
        auto initialize = [&]
        {
            pins.initializeNewTable(CreateTablePayload{"pins", "test", schema.columns, schema.constraints});
        };
        initialize();
        {
            const auto first = pins.getHeaderPage();
            {
                const auto second = pins.getHeaderPage();
                requireThrows(initialize, "pinned");
            }
            requireThrows(initialize, "pinned");
        }
        initialize();
        {
            std::optional<PageGuard> source{pins.getHeaderPage()};
            std::optional<PageGuard> moved{std::move(*source)};
            source.reset();
            requireThrows(initialize, "pinned");
            moved.reset();
            initialize();
        }
        {
            auto source = pins.getHeaderPage();
            auto target = pins.getPage<DataPage>(1);
            target = std::move(source);
            requireThrows(initialize, "pinned");
            auto &self = target;
            target = std::move(self);
            require(target.page().pageId() == 0, "Self-move lost the page guard");
        }
        initialize();
        BufferManager otherPins{LinuxDirectory{path / "tables"}, "other-pins.table", LinuxFile::OpenMode::CreateNew, recovery};
        auto initializeOther = [&]
        {
            otherPins.initializeNewTable(CreateTablePayload{"other-pins", "test", schema.columns, schema.constraints});
        };
        initializeOther();
        {
            auto source = pins.getHeaderPage();
            auto target = otherPins.getHeaderPage();
            target = std::move(source);
            initializeOther();
            requireThrows(initialize, "pinned");
        }
        initialize();

        requireThrows(
            [&]
            {
                auto guard = pins.getHeaderPage();
                throw std::runtime_error("unwind test");
            },
            "unwind test");
        initialize();

        // Both guards acquired by getPage<DataPage> must release on validation failure.
        requireThrows([&]
                      { pins.getPage<DataPage>(0); }, "not a DataPage");
        initialize();

        BufferManager invalidHeader{LinuxDirectory{path / "tables"}, relativeFile.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
        invalidHeader.getPage(0, [](const RawPage &)
                              { return makeEmptyDataPage(0); });
        requireThrows([&]
                      { invalidHeader.getHeaderPage(); }, "not a header page");
        requireThrows([&]
                      { invalidHeader.initializeNewTable(CreateTablePayload{"guard_values", "test", schema.columns, schema.constraints}); }, "new, unwritten table");

        BufferManager decodeFailure{LinuxDirectory{path / "tables"}, relativeFile.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
        requireThrows(
            [&]
            {
                decodeFailure.getPage(1, [](const RawPage &) -> Page
                                      { throw std::runtime_error("decode failure"); });
            },
            "decode failure");
        decodeFailure.getPage<DataPage>(1);
        requireThrows([&]
                      { decodeFailure.initializeNewTable(CreateTablePayload{"guard_values", "test", schema.columns, schema.constraints}); }, "new, unwritten table");
        recovery.rollback();
    }

    void testCursors(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE cursor_values (id INT, label TEXT);");
        const std::filesystem::path relativeFile = "tables/cursor_values.table";
        const journal_test::DatabaseIdentity identity{path};
        StatementRecovery recovery{LinuxDirectory{path}, identity.bytes};
        {
            Table table = Table::open(LinuxDirectory{path / "tables"}, relativeFile.filename(), recovery);
            require(scanIds(table).empty(), "An empty table returned rows");
        }
        const std::string label(1500, 'x');
        std::string insertSql = "INSERT INTO cursor_values VALUES ";
        for (std::int32_t id = 1; id <= 6; ++id)
        {
            if (id > 1)
                insertSql += ", ";
            insertSql += "(" + std::to_string(id) + ", '" + label + "')";
        }
        database.executeSql(insertSql + ";");
        {
            BufferManager manager{LinuxDirectory{path / "tables"}, relativeFile.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
            auto first = manager.getPage<DataPage>(1);
            require(first.as<DataPage>().rows.size() == 2 && first.page().nextPageId() != 0,
                    "Cursor fixture did not span multiple pages");
        }

        std::optional<Row> retained;
        {
            Table table = Table::open(LinuxDirectory{path / "tables"}, relativeFile.filename(), recovery);
            require(scanIds(table) == std::vector<std::int32_t>{1, 2, 3, 4, 5, 6},
                    "Cursor did not traverse all pages in order");
            {
                auto first = table.scan();
                auto second = table.scan();
                retained = first.next();
                require(retained.has_value(), "Cursor returned no first row");
                require(nextId(first) == 2 && nextId(second) == 1 && nextId(second) == 2,
                        "Independent cursors interfered with each other");
                while (first.next())
                {
                }
                require(std::get<std::string>(retained->values.at(1)) == label,
                        "Advancing the cursor invalidated an owned row");
            }
            {
                auto source = table.scan();
                require(nextId(source) == 1, "Cursor move fixture is incorrect");
                auto moved = std::move(source);
                require(!source.next() && nextId(moved) == 2,
                        "Cursor move construction lost its position");
                auto target = table.scan();
                require(nextId(target) == 1, "Cursor assignment fixture is incorrect");
                target = std::move(moved);
                require(!moved.next() && nextId(target) == 3,
                        "Cursor move assignment lost its position");
                auto &self = target;
                target = std::move(self);
                require(nextId(target) == 4, "Cursor self-move lost its position");
                // Destruction here abandons the remaining rows on pinned pages.
            }

            // The first page now has a hole; the middle page is entirely empty.
            for (std::int32_t id : {1, 3, 4})
            {
                deleteId(table, recovery, id);
            }
            require(scanIds(table) == std::vector<std::int32_t>{2, 5, 6},
                    "Cursor confused compact rows with slots or stopped at an empty page");
            recovery.begin();
            table.insertRows(BoundInsert{
                .tableName = "cursor_values",
                .rows = {Row{{std::int32_t{99}, label}}}});
            recovery.commit();
            auto ids = scanIds(table);
            std::sort(ids.begin(), ids.end());
            require(ids == std::vector<std::int32_t>{2, 5, 6, 99},
                    "Cursor mishandled a reused slot in a cached page");
        }
        require(std::get<std::int32_t>(retained->values.at(0)) == 1 &&
                    std::get<std::string>(retained->values.at(1)) == label,
                "Destroying the table invalidated a returned row");

        Table reopened = Table::open(LinuxDirectory{path / "tables"}, relativeFile.filename(), recovery);
        auto ids = scanIds(reopened);
        std::sort(ids.begin(), ids.end());
        require(ids == std::vector<std::int32_t>{2, 5, 6, 99},
                "Deleted and reused slots did not survive reopening");
        require(database.executeSql("SELECT * FROM cursor_values;").rows.size() == 4,
                "SQL SELECT disagrees with the storage cursor after slot reuse");
    }

    void testLocatedDeletes(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE delete_values (id INT);");
        database.executeSql("INSERT INTO delete_values VALUES (1), (2), (3);");
        const journal_test::DatabaseIdentity identity{path};
        StatementRecovery recovery{LinuxDirectory{path}, identity.bytes};
        std::vector<LocatedRow> rows;
        {
            auto table = Table::open(LinuxDirectory{path / "tables"}, "delete_values.table", recovery);
            auto cursor = table.scan();
            while (auto row = cursor.nextLocated())
                rows.push_back(std::move(*row));
        }

        recovery.begin();
        {
            BufferManager manager{LinuxDirectory{path / "tables"}, "delete_values.table",
                                  LinuxFile::OpenMode::OpenExisting, recovery};
            require(manager.deleteRow(rows[0].id) == 1, "Single-row deletion returned the wrong count");
            require(manager.getHeaderPage().as<HeaderPage>().totalRowCount == 2,
                    "Single-row deletion did not decrement totalRowCount");
            // Slot 2 is no longer at vector index 2 after deleting slot 0.
            const std::vector<RowId> ids{rows[2].id, rows[2].id, rows[0].id};
            require(manager.deleteRows(ids) == 1, "Duplicate/deleted IDs were counted more than once");
            require(manager.getHeaderPage().as<HeaderPage>().totalRowCount == 1,
                    "Batch deletion decremented totalRowCount incorrectly");
            require(manager.deleteRows(std::vector<RowId>{}) == 0,
                    "Empty buffer-manager deletion returned a nonzero count");
            manager.flushAll();
            manager.sync();
        }
        recovery.commit();

        recovery.begin();
        {
            auto table = Table::open(LinuxDirectory{path / "tables"}, "delete_values.table", recovery);
            require(scanIds(table) == std::vector<std::int32_t>{2},
                    "Deleted rows remained visible after reopening");
            require(table.deleteRows({}) == 0, "Empty table deletion returned a nonzero count");
            require(table.deleteRows({rows[0], rows[0], rows[2]}) == 0,
                    "Repeated table deletion removed or counted another row");
            table.insertRows({.tableName = "delete_values", .rows = {Row{{std::int32_t{4}}}}});
        }
        recovery.commit();

        auto reopened = Table::open(LinuxDirectory{path / "tables"}, "delete_values.table", recovery);
        auto remaining = scanIds(reopened);
        std::sort(remaining.begin(), remaining.end());
        require(remaining == std::vector<std::int32_t>{2, 4},
                "Slot reuse after deletion lost or duplicated rows");
        const auto header = decodeHeaderPage(readPageFromFile(
            LinuxDirectory{path / "tables"}, "delete_values.table", HeaderPageId));
        require(std::get<HeaderPage>(header.data).totalRowCount == 2,
                "Row count after deletion and slot reuse did not persist");
    }

    void testLocatedUpdates(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE update_values (id INT, label TEXT);");
        database.executeSql("INSERT INTO update_values VALUES (1, 'old1'), (2, 'old2'), (3, 'unaffected');");
        const journal_test::DatabaseIdentity identity{path};
        StatementRecovery recovery{LinuxDirectory{path}, identity.bytes};
        recovery.begin();
        {
            auto table = Table::open(LinuxDirectory{path / "tables"}, "update_values.table", recovery);
            std::vector<LocatedRow> replacements;
            {
                auto cursor = table.scan();
                while (auto row = cursor.nextLocated())
                {
                    if (std::get<std::int32_t>(row->row.values[0]) <= 2)
                        replacements.push_back(std::move(*row));
                }
            }
            replacements[0].row.values[1] = std::string(3000, 'a');
            replacements[1].row.values[1] = std::string(1500, 'b');
            table.updateRows(replacements);
        }
        recovery.commit();

        const auto rows = database.executeSql("SELECT * FROM update_values;").rows;
        require(rows.size() == 3, "UPDATE inserted duplicates instead of replacing rows");
        for (const auto &row : rows)
        {
            const auto id = std::get<std::int32_t>(row.values[0]);
            const auto &label = std::get<std::string>(row.values[1]);
            require((id == 1 && label == std::string(3000, 'a')) ||
                        (id == 2 && label == std::string(1500, 'b')) ||
                        (id == 3 && label == "unaffected"),
                    "UPDATE changed the wrong row or retained an original value");
        }
        const auto header = decodeHeaderPage(readPageFromFile(
            LinuxDirectory{path / "tables"}, "update_values.table", HeaderPageId));
        const auto &schema = std::get<HeaderPage>(header.data);
        require(schema.totalRowCount == 3 && schema.firstDataPageId != schema.lastDataPageId,
                "Growing UPDATE lost row count or did not span multiple pages");
    }

    void testLazyLoading(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE lazy_values (id INT);");
        database.executeSql("INSERT INTO lazy_values VALUES (1), (2);");
        const std::filesystem::path relativeFile = "tables/lazy_values.table";
        const journal_test::DatabaseIdentity identity{path};
        StatementRecovery recovery{LinuxDirectory{path}, identity.bytes};
        recovery.begin();
        {
            BufferManager manager{LinuxDirectory{path / "tables"}, relativeFile.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
            auto page = manager.getPage<DataPage>(1);
            page.writePage().header.nextPageId = 50; // Deliberately missing next page.
            manager.flushPage(1);
            manager.sync();
        }
        recovery.commit();
        Table table = Table::open(LinuxDirectory{path / "tables"}, relativeFile.filename(), recovery);
        {
            auto cursor = table.scan();
            require(nextId(cursor) == 1, "Cursor eagerly read a later page");
        }
        auto cursor = table.scan();
        require(nextId(cursor) == 1 && nextId(cursor) == 2,
                "Cursor did not finish its current page before loading the next");
        requireThrows([&]
                      { cursor.next(); }, "read");
    }

    void testLargePageIds(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE high_page_values (id INT);");
        const std::filesystem::path relativeFile = "tables/high_page_values.table";
        const auto file = path / relativeFile;
        constexpr PageId highId = 70000;
        // Build the sparse physical-page fixture without accessing setPage().
        {
            const auto header = decodeHeaderPage(readPageFromFile(LinuxDirectory{file.parent_path()}, file.filename(), 0));
            const auto page = makeEmptyDataPage(highId);
            PageFile pages{LinuxDirectory{file.parent_path()}, file.filename()};
            pages.writePage(highId, encodeDataPage(page.header,
                                                   std::get<HeaderPage>(header.data), std::get<DataPage>(page.data)));
            pages.sync();
        }
        const journal_test::DatabaseIdentity identity{path};
        StatementRecovery recovery{LinuxDirectory{path}, identity.bytes};
        recovery.begin();
        {
            BufferManager manager{LinuxDirectory{path / "tables"}, relativeFile.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
            auto header = manager.getHeaderPage();
            auto &metadata = header.write<HeaderPage>();
            metadata.firstDataPageId = highId;
            metadata.lastDataPageId = highId;
            metadata.nextUnusedPageId = highId + 1;
            manager.insertRows({Row{{std::int32_t{42}}}});
            metadata.firstDataPageId = 1;
            auto first = manager.getPage<DataPage>(1);
            first.writePage().header.nextPageId = highId;
            manager.flushAll();
            manager.sync();
        }
        recovery.commit();
        Table table = Table::open(LinuxDirectory{path / "tables"}, relativeFile.filename(), recovery);
        require(scanIds(table) == std::vector<std::int32_t>{42},
                "A page ID above 65535 was truncated during scanning");
    }
}

int main()
{
    static_assert(!std::is_copy_constructible_v<PageGuard>);
    static_assert(!std::is_copy_assignable_v<PageGuard>);
    static_assert(std::is_nothrow_move_constructible_v<PageGuard>);
    static_assert(std::is_nothrow_move_assignable_v<PageGuard>);
    static_assert(std::is_nothrow_destructible_v<PageGuard>);
    static_assert(std::is_same_v<decltype(std::declval<PageGuard &>().page()), const Page &>);
    static_assert(std::is_same_v<decltype(std::declval<PageGuard &>().as<DataPage>()), const DataPage &>);
    static_assert(!std::is_copy_constructible_v<TableCursor>);
    static_assert(!std::is_copy_assignable_v<TableCursor>);
    static_assert(std::is_nothrow_move_constructible_v<TableCursor>);
    static_assert(std::is_nothrow_move_assignable_v<TableCursor>);
    static_assert(std::is_same_v<decltype(std::declval<Table &>().scan()), TableCursor>);

    TestDirectory directory{"db-storage-cursor-test"};
    const auto path = directory.path / "db";
    Database database = Database::create(path, "db_storage_cursor_test");
    testPageGuards(database, path);
    testCursors(database, path);
    testLocatedDeletes(database, path);
    testLocatedUpdates(database, path);
    testLazyLoading(database, path);
    testLargePageIds(database, path);
}
