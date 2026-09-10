#include "db_buffer_manager.h"
#include "db_database.h"
#include "db_page_factory.h"
#include "db_table.h"

#include <algorithm>
#include <chrono>
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
        catch (const std::runtime_error &error)
        {
            require(std::string{error.what()}.find(expected) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected error containing: " + expected);
    }

    struct TestDirectory
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("db_storage_cursor_test_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));

        TestDirectory()
        {
            require(std::filesystem::create_directory(path),
                    "Could not create a fresh test directory");
        }

        ~TestDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

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

    void deleteId(Table &table, std::int32_t id)
    {
        BoundDelete query{
            .tableName = "cursor_values",
            .where = std::make_unique<BoundBinaryExpr>(
                BinaryOperator::Eq,
                std::make_unique<BoundColumnExpr>(0, DataType::Int),
                std::make_unique<BoundLiteralExpr>(Value{id}, DataType::Int),
                DataType::Boolean)};
        require(table.deleteRows(query) == 1, "DELETE did not remove the expected row");
    }

    void testPageGuards(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE guard_values (id INT);");
        database.executeSql("INSERT INTO guard_values VALUES (1);");
        const auto file = path / "tables" / "guard_values.table";
        BufferManager manager{file};
        Page headerSnapshot = manager.getHeaderPage().page();
        Page dataSnapshot = makeEmptyDataPage(1);
        {
            auto first = manager.getDataPage(1);
            {
                const auto second = manager.getDataPage(1);
                require(&first.page() == &second.page(),
                        "Guards do not reference the same cached page");
                first.as<DataPage>().rows.at(0).row.values.at(0) = std::int32_t{42};
                first.markDirty();
                require(std::get<std::int32_t>(second.as<DataPage>().rows.at(0).row.values.at(0)) == 42,
                        "Guard writes were not visible through another guard");
                dataSnapshot = first.page();
                requireThrows([&] { manager.setPage(dataSnapshot, 1); }, "pinned");
            }
            requireThrows([&] { manager.setPage(dataSnapshot, 1); }, "pinned");

            // Grow the map while keeping a page pinned: rehash must not move it.
            const Page *address = &first.page();
            for (PageId id = 2; id < 128; ++id)
            {
                manager.setPage(makeEmptyDataPage(id), id);
            }
            const auto again = manager.getDataPage(1);
            require(&again.page() == address, "Cache growth invalidated a guard");
            manager.flushPage(1);
        }
        manager.setPage(dataSnapshot, 1);
        manager.setPage(headerSnapshot, 0);

        BufferManager reopened{file};
        require(std::get<std::int32_t>(reopened.getDataPage(1).as<DataPage>().rows.at(0).row.values.at(0)) == 42,
                "Dirty page changes did not survive reopening");

        {
            std::optional<PageGuard> source{manager.getDataPage(2)};
            std::optional<PageGuard> moved{std::move(*source)};
            source.reset();
            requireThrows([&] { manager.setPage(makeEmptyDataPage(2), 2); }, "pinned");
            moved.reset();
            manager.setPage(makeEmptyDataPage(2), 2);
        }
        {
            auto source = manager.getDataPage(2);
            auto target = manager.getDataPage(3);
            target = std::move(source);
            manager.setPage(makeEmptyDataPage(3), 3);
            requireThrows([&] { manager.setPage(makeEmptyDataPage(2), 2); }, "pinned");
            auto &self = target;
            target = std::move(self);
            require(target.page().pageId() == 2, "Self-move lost the page guard");
        }
        manager.setPage(makeEmptyDataPage(2), 2);
        {
            auto source = manager.getDataPage(1);
            auto target = reopened.getDataPage(1);
            target = std::move(source);
            reopened.setPage(dataSnapshot, 1);
            requireThrows([&] { manager.setPage(dataSnapshot, 1); }, "pinned");
        }
        manager.setPage(dataSnapshot, 1);

        requireThrows(
            [&]
            {
                auto guard = manager.getDataPage(2);
                throw std::runtime_error("unwind test");
            },
            "unwind test");
        manager.setPage(makeEmptyDataPage(2), 2);

        // Both guards acquired by getDataPage must release on validation failure.
        requireThrows([&] { manager.getDataPage(0); }, "not a data page");
        manager.setPage(headerSnapshot, 0);
        manager.setPage(makeEmptyDataPage(0), 0);
        requireThrows([&] { manager.getHeaderPage(); }, "not a table header");
        manager.setPage(headerSnapshot, 0);

        BufferManager decodeFailure{file};
        requireThrows(
            [&]
            {
                decodeFailure.getPage(1, [](const RawPage &) -> Page
                {
                    throw std::runtime_error("decode failure");
                });
            },
            "decode failure");
        decodeFailure.getDataPage(1);
        decodeFailure.setPage(dataSnapshot, 1);
    }

    void testCursors(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE cursor_values (id INT, label TEXT);");
        const auto file = path / "tables" / "cursor_values.table";
        {
            Table table = Table::open(file);
            require(scanIds(table).empty(), "An empty table returned rows");
        }
        const std::string label(1500, 'x');
        std::vector<Row> rows;
        for (std::int32_t id = 1; id <= 6; ++id)
        {
            rows.push_back(Row{{id, label}});
        }
        database.insertRows("cursor_values", rows);
        {
            BufferManager manager{file};
            auto first = manager.getDataPage(1);
            require(first.as<DataPage>().rows.size() == 2 && first.page().nextPageId() != 0,
                    "Cursor fixture did not span multiple pages");
        }

        std::optional<Row> retained;
        {
            Table table = Table::open(file);
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
                deleteId(table, id);
            }
            require(scanIds(table) == std::vector<std::int32_t>{2, 5, 6},
                    "Cursor confused compact rows with slots or stopped at an empty page");
            table.insertRows(BoundInsert{
                .tableName = "cursor_values",
                .rows = {Row{{std::int32_t{99}, label}}}});
            auto ids = scanIds(table);
            std::sort(ids.begin(), ids.end());
            require(ids == std::vector<std::int32_t>{2, 5, 6, 99},
                    "Cursor mishandled a reused slot in a cached page");
        }
        require(std::get<std::int32_t>(retained->values.at(0)) == 1 &&
                    std::get<std::string>(retained->values.at(1)) == label,
                "Destroying the table invalidated a returned row");

        Table reopened = Table::open(file);
        auto ids = scanIds(reopened);
        std::sort(ids.begin(), ids.end());
        require(ids == std::vector<std::int32_t>{2, 5, 6, 99},
                "Deleted and reused slots did not survive reopening");
        require(database.selectAllRows("cursor_values").size() == 4,
                "The vector convenience scan changed behavior");
    }

    void testLazyLoading(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE lazy_values (id INT);");
        database.executeSql("INSERT INTO lazy_values VALUES (1), (2);");
        const auto file = path / "tables" / "lazy_values.table";
        {
            BufferManager manager{file};
            auto page = manager.getDataPage(1);
            page.page().header.nextPageId = 50; // Deliberately missing next page.
            page.markDirty();
            manager.flushPage(1);
        }
        Table table = Table::open(file);
        {
            auto cursor = table.scan();
            require(nextId(cursor) == 1, "Cursor eagerly read a later page");
        }
        auto cursor = table.scan();
        require(nextId(cursor) == 1 && nextId(cursor) == 2,
                "Cursor did not finish its current page before loading the next");
        requireThrows([&] { cursor.next(); }, "read");
    }

    void testLargePageIds(Database &database, const std::filesystem::path &path)
    {
        database.executeSql("CREATE TABLE high_page_values (id INT);");
        const auto file = path / "tables" / "high_page_values.table";
        constexpr PageId highId = 70000;
        {
            BufferManager manager{file};
            manager.setPage(makeEmptyDataPage(highId), highId);
            auto header = manager.getHeaderPage();
            auto &metadata = header.as<HeaderPage>();
            metadata.firstDataPageId = highId;
            metadata.lastDataPageId = highId;
            metadata.nextUnusedPageId = highId + 1;
            manager.insertAllRows({Row{{std::int32_t{42}}}});
            metadata.firstDataPageId = 1;
            header.markDirty();
            auto first = manager.getDataPage(1);
            first.page().header.nextPageId = highId;
            first.markDirty();
            // Seeking to the high page creates a sparse file on supported systems.
            manager.flushAll();
        }
        Table table = Table::open(file);
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
    static_assert(!std::is_copy_constructible_v<TableCursor>);
    static_assert(!std::is_copy_assignable_v<TableCursor>);
    static_assert(std::is_nothrow_move_constructible_v<TableCursor>);
    static_assert(std::is_nothrow_move_assignable_v<TableCursor>);
    static_assert(std::is_same_v<decltype(std::declval<Table &>().scan()), TableCursor>);

    TestDirectory directory;
    Database database{directory.path, "db_storage_cursor_test"};
    testPageGuards(database, directory.path);
    testCursors(database, directory.path);
    testLazyLoading(database, directory.path);
    testLargePageIds(database, directory.path);
}
