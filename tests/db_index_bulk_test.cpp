#include "db_database.h"
#include "db_index.h"
#include "db_index_codec.h"
#include "db_page_factory.h"
#include "db_read.h"
#include "db_test_directory.h"
#include "db_test_index.h"
#include "db_test_journal.h"
#include "db_write.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace
{
    bool failTemporaryWrite = false;
    bool failTemporaryRead = false;
    constexpr IndexBulkLoadOptions SmallSort{32 * 1024, 2};

    void require(bool condition, const std::string &message)
    {
        if (!condition) throw std::runtime_error(message);
    }

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

    using index_test::same;
    using index_test::Tree;

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
                .keyColumns = {1, 2}, .columns = buildStoredColumns({text, number}), .unique = unique};
    }

    LeafIndexEntry generated(std::size_t i)
    {
        const auto number = static_cast<std::int32_t>(i);
        return {Row{{std::string(550, 'x') + std::to_string(i), number}},
                RowId{static_cast<PageId>(i + 1), 0}};
    }

    void testTableBulkLoad()
    {
        TestDirectory directory{"db-index-bulk"};
        const auto root = directory.path / "db";
        constexpr std::size_t Count = 1600;
        {
            auto database = Database::create(root, "test");
            database.executeSql("CREATE TABLE items (id INT, text TEXT, number INT);");
            std::string insert = "INSERT INTO items VALUES ";
            for (std::size_t i = 0; i < Count; ++i)
            {
                if (i) insert += ",";
                const auto text = i % 17 == 0 ? "NULL" : "'k" + std::to_string((Count - i) % 13) +
                    std::string(200 + (i % 5) * 140, 'a') + "'";
                insert += "(" + std::to_string(i) + "," + text + "," +
                          std::to_string(static_cast<int>(i % 9) - 4) + ")";
            }
            database.executeSql(insert + ";");
            database.executeSql("DELETE FROM items WHERE id = 19;"); // Preserve physical RowIds and skip tombstones.
        }
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        std::vector<LeafIndexEntry> expected;
        {
            auto table = Table::open(LinuxDirectory(root / "tables"), "items.table", recovery);
            auto cursor = table.scan();
            while (auto row = cursor.nextLocated())
                expected.push_back({Row{{row->row.values[1], row->row.values[2]}}, row->id});
        }
        // Independent reference ordering for this nullable TEXT / INT schema.
        std::sort(expected.begin(), expected.end(), [](const auto &a, const auto &b) {
            const auto &x = a.key.values[0];
            const auto &y = b.key.values[0];
            if (x.index() != y.index()) return std::holds_alternative<std::monostate>(x);
            if (const auto *text = std::get_if<std::string>(&x))
            {
                const auto &other = std::get<std::string>(y);
                if (*text != other) return *text < other;
            }
            const auto n = std::get<std::int32_t>(a.key.values[1]);
            const auto m = std::get<std::int32_t>(b.key.values[1]);
            return n != m ? n < m : a.rowId < b.rowId;
        });
        recovery.begin();
        IndexBulkLoadStats stats;
        {
            auto table = Table::open(LinuxDirectory(root / "tables"), "items.table", recovery);
            auto index = Index::create(LinuxDirectory(root / "indexes"), payload("bulk"), recovery);
            stats = index.bulkLoad(table, SmallSort);
        }
        recovery.commit();
        require(stats.rows == Count - 1 && stats.initialRuns > 4 && stats.mergePasses > 1,
                "Fixture did not exercise multi-pass external merging");
        Tree tree(root, "bulk");
        require(tree.height >= 2 && tree.leaves.size() > 64, "Fixture did not exercise a large multi-level tree");
        require(tree.entries.size() == expected.size(), "Lost or duplicated entries");
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(same(tree.entries[i], expected[i]), "Wrong key order or table RowId");
        tree.verifyRouting();
        require(tree.metadata.columns[0].columnIndex == 0 && tree.metadata.columns[1].columnIndex == 1,
                "Index schema was not normalized");
        require(stats.leafPages == tree.leaves.size() && stats.internalPages + stats.leafPages == tree.visited.size(),
                "Page statistics mismatch");
        {
            auto opened = Index::open(LinuxDirectory(root / "indexes"), "bulk.index", recovery);
            (void)opened;
            BufferManager manager(LinuxDirectory(root / "indexes"), "bulk.index", LinuxFile::OpenMode::OpenExisting,
                                  recovery, std::nullopt, true);
            require(manager.getPage<IndexPage>(tree.metadata.rootPageId).as<IndexPage>().header.level == tree.height,
                    "Reopened index did not load its key schema");
        }
    }

    void testParentTailRedistribution()
    {
        TestDirectory directory{"db-index-tail"};
        const auto root = directory.path / "db";
        { auto database = Database::create(root, "test"); }
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        recovery.begin();
        {
            auto index = Index::create(LinuxDirectory(root / "indexes"), payload("tail"), recovery);
            std::size_t position = 0;
            index.bulkLoad([&]() -> std::optional<LeafIndexEntry> {
                if (position == 8) return std::nullopt;
                auto entry = generated(position++);
                entry.key.values[0] = std::string(1900, 'x');
                return entry;
            }, SmallSort);
        }
        recovery.commit();
        Tree tree(root, "tail");
        require(tree.height == 2 && tree.leaves.size() == 4 && tree.entries.size() == 8,
                "Fixture did not exercise trailing internal-child redistribution");
        tree.verifyRouting();
    }

    void testSqlAndEmpty()
    {
        TestDirectory directory{"db-index-sql"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "test");
            db.executeSql("CREATE TABLE items (id INT, text TEXT, number INT);");
            db.executeSql("CREATE INDEX empty_index ON items (text, number);");
            Tree empty(root, "empty_index");
            require(empty.entries.empty() && empty.height == 0 && empty.leaves.size() == 1, "Invalid empty root");
            db.executeSql("INSERT INTO items VALUES (1, 'b', -2), (2, 'a', 9), (3, NULL, 5), (4, NULL, 5);");
            db.executeSql("CREATE UNIQUE INDEX populated ON items (text, number);");
            requireError([&] { db.executeSql("CREATE INDEX populated ON items (id);"); }, "already exists");
            db.executeSql("CREATE INDEX reversed ON items (number, text);");
        }
        Tree maintained(root, "empty_index");
        require(maintained.entries.size() == 4, "INSERT did not maintain the existing index");
        Tree populated(root, "populated");
        require(populated.entries.size() == 4 && populated.metadata.unique, "SQL CREATE INDEX did not populate entries");
        Tree reversed(root, "reversed");
        require(reversed.metadata.keyColumns == std::vector<ColumnId>{2, 1} &&
                std::get<std::int32_t>(reversed.entries.front().key.values[0]) == -2,
                "Index-column order was not preserved");
    }

    std::size_t descriptorCount()
    {
        return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                                                     std::filesystem::directory_iterator{}));
    }

    void testFailuresAndRecovery()
    {
        TestDirectory directory{"db-index-failure"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "test");
            db.executeSql("CREATE TABLE collision (id INT);");
            db.executeSql("INSERT INTO collision VALUES (42);");
        }
        const auto before = journal_test::readFile(root / "tables/collision.table");
        const journal_test::DatabaseIdentity identity{root};
        for (const std::string failure : {"duplicate", "source", "write", "read", "oversize", "budget"})
        {
            StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
            recovery.begin();
            const auto descriptors = descriptorCount();
            {
                auto index = Index::create(LinuxDirectory(root / "indexes"), payload("collision", true), recovery);
                std::size_t position = 0;
                failTemporaryWrite = failure == "write";
                failTemporaryRead = failure == "read";
                auto source = [&]() -> std::optional<LeafIndexEntry> {
                    if (failure == "source" && position == 200) throw std::runtime_error("source failure");
                    if (position == 600) return std::nullopt;
                    auto entry = generated(position);
                    if (failure == "duplicate" && position == 599) entry.key = generated(598).key;
                    if (failure == "oversize") entry.key.values[0] = std::string(3000, 'x');
                    ++position;
                    return entry;
                };
                const std::string error = failure == "duplicate" ? "UNIQUE INDEX" :
                    failure == "source" ? "source failure" : failure == "oversize" ? "too large" :
                    failure == "budget" ? "merge heads" : "temporary file";
                requireError([&] { index.bulkLoad(source, failure == "budget" ? IndexBulkLoadOptions{1, 2} : SmallSort); }, error);
                failTemporaryWrite = false;
                failTemporaryRead = false;
                requireError([&] { recovery.commit(); }, "failed");
            }
            require(descriptorCount() == descriptors, "Temporary file descriptor leak after failed build");
            recovery.rollback();
            require(!std::filesystem::exists(root / "indexes/collision.index"), "Rollback left a partial index");
            require(journal_test::readFile(root / "tables/collision.table") == before, "Index rollback touched the table");
        }
        // Leave durable, uncommitted work behind, then exercise startup recovery.
        {
            StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
            recovery.begin();
            auto index = Index::create(LinuxDirectory(root / "indexes"), payload("abandoned"), recovery);
            std::size_t position = 0;
            index.bulkLoad([&]() -> std::optional<LeafIndexEntry> {
                if (position == 200) return std::nullopt;
                return generated(position++);
            }, SmallSort);
        }
        require(std::filesystem::exists(root / "indexes/abandoned.index"), "Missing recovery fixture");
        { auto engine = StorageEngine::open(root); }
        require(!std::filesystem::exists(root / "indexes/abandoned.index"), "Startup recovery left an uncommitted index");
        require(journal_test::readFile(root / "tables/collision.table") == before, "Startup recovery damaged table");
    }

    void testTypedKeysAndCorruption()
    {
        require(compareIndexKeys(Row{{std::int32_t{-10}}}, Row{{std::int32_t{1}}}) < 0, "Signed integer ordering");
        require(compareIndexKeys(Row{{DecimalValue{100, 2}}}, Row{{DecimalValue{1, 0}}}) == 0, "Decimal scale ordering");
        require(compareIndexKeys(Row{{std::float64_t{-0.0}}}, Row{{std::float64_t{0.0}}}) == 0, "Signed zero ordering");
        require(compareIndexKeys(Row{{std::numeric_limits<std::float64_t>::quiet_NaN()}}, Row{{std::float64_t{1.0}}}) > 0,
                "NaN ordering is not total");
        const RowSchema schema{payload("codec").columns};
        auto page = makeEmptyPage<IndexPage>(1);
        std::get<std::vector<LeafIndexEntry>>(std::get<IndexPage>(page.data).entries).push_back(generated(1));
        auto bytes = encodeIndexPage(page, schema);
        require(same(std::get<std::vector<LeafIndexEntry>>(std::get<IndexPage>(decodeIndexPage(bytes, schema).data).entries)[0], generated(1)),
                "Index entry round trip");
        bytes.bytes[PageHeaderLayout::Size + IndexPageHeader::serializedSize()] = std::byte{0xff};
        requireError([&] { decodeIndexPage(bytes, schema); }, "entry size");
    }
}

extern "C" ssize_t __wrap_pwrite(int fd, const void *data, size_t size, off_t offset)
{
    struct stat info{};
    if (failTemporaryWrite && ::fstat(fd, &info) == 0 && info.st_nlink == 0)
    {
        failTemporaryWrite = false;
        errno = ENOSPC;
        return -1;
    }
    return static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, size, offset));
}

extern "C" ssize_t __wrap_pread(int fd, void *data, size_t size, off_t offset)
{
    struct stat info{};
    if (failTemporaryRead && ::fstat(fd, &info) == 0 && info.st_nlink == 0)
    {
        failTemporaryRead = false;
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, size, offset));
}

int main()
{
    try
    {
        testTableBulkLoad();
        testSqlAndEmpty();
        testParentTailRedistribution();
        testFailuresAndRecovery();
        testTypedKeysAndCorruption();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
