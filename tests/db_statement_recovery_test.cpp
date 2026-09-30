#include "db_database.h"
#include "db_journal_file.h"
#include "db_statement_recovery.h"
#include "db_storage_engine.h"
#include "db_test_directory.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
    using Path = std::filesystem::path;

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <typename Function>
    void requireError(Function function, const std::string &message)
    {
        try
        {
            function();
        }
        catch (const std::exception &error)
        {
            require(std::string{error.what()}.find(message) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected error: " + message);
    }

    struct Event
    {
        std::string operation;
        Path path;
        bool tableExists;
        bool journalExists;
    };

    struct Probe
    {
        Path root;
        Path target;
        std::string failOperation;
        Path failPath;
        unsigned skipMatches = 0;
        bool failed = false;
        std::vector<Event> events;
    };

    Probe *activeProbe = nullptr;

    struct Observe
    {
        explicit Observe(Probe &probe) { activeProbe = &probe; }
        ~Observe() { activeProbe = nullptr; }
    };

    bool observe(const std::string &operation, const Path &path)
    {
        if (!activeProbe)
            return false;
        auto &probe = *activeProbe;
        probe.events.push_back(Event{operation, path,
            std::filesystem::exists(probe.target),
            std::filesystem::exists(probe.root / "journal/journal.log")});
        if (!probe.failed && operation == probe.failOperation && path == probe.failPath)
        {
            if (probe.skipMatches != 0)
            {
                --probe.skipMatches;
                return false;
            }
            probe.failed = true;
            errno = EIO;
            return true;
        }
        return false;
    }

    Path descriptorPath(int fd)
    {
        char target[4096];
        const auto link = std::string{"/proc/self/fd/"} + std::to_string(fd);
        const auto size = ::readlink(link.c_str(), target, sizeof(target));
        return size < 0 ? Path{} : Path{std::string{target, static_cast<std::size_t>(size)}};
    }

    std::vector<std::byte> readFile(const Path &path)
    {
        LinuxFile file{path, LinuxFile::OpenMode::ReadOnly};
        std::vector<std::byte> bytes(static_cast<std::size_t>(file.size()));
        file.readExactAt(0, bytes);
        return bytes;
    }

    std::size_t eventIndex(const Probe &probe, const std::string &operation, const Path &path)
    {
        const auto event = std::find_if(probe.events.begin(), probe.events.end(),
            [&](const Event &value) { return value.operation == operation && value.path == path; });
        require(event != probe.events.end(), "Missing event: " + operation + " " + path.string());
        return static_cast<std::size_t>(event - probe.events.begin());
    }

    void testCreationFailures()
    {
        // All failures are one-shot so rollback can make progress. The pwrite
        // case fails after one database page has already been written.
        for (const std::string boundary : {"journal-sync", "journal-dir-sync", "create", "write", "table-sync"})
        {
            TestDirectory directory{"db-create-failure"};
            const Path root = directory.path / "db";
            auto database = Database::create(root, "test");
            const Path target = root / "tables/items.table";
            const Path journal = root / "journal/journal.log";
            Probe probe{};
            probe.root = root;
            probe.target = target;
            probe.failOperation = boundary == "create" ? "create" : boundary == "write" ? "write" : "sync";
            probe.failPath = boundary == "journal-sync" ? journal :
                             boundary == "journal-dir-sync" ? root / "journal" : target;
            probe.skipMatches = boundary == "write" ? 1 : 0;
            {
                Observe observe{probe};
                requireError([&] { database.executeSql("CREATE TABLE items (id INT);"); }, "Input/output error");
            }
            require(probe.failed, "Fault injection was not reached: " + boundary);
            require(!std::filesystem::exists(target), "Failed CREATE retained its table: " + boundary);
            require(!std::filesystem::exists(journal), "Failed CREATE retained its journal: " + boundary);
            const auto directorySync = eventIndex(probe, "sync", root / "tables");
            require(!probe.events[directorySync].tableExists && probe.events[directorySync].journalExists,
                    "Rollback must sync table deletion before retiring the journal");
            if (boundary == "create" || boundary == "write" || boundary == "table-sync")
            {
                const auto creation = eventIndex(probe, "create", target);
                require(eventIndex(probe, "sync", journal) < creation &&
                        eventIndex(probe, "sync", root / "journal") < creation,
                        "Table creation preceded durable nonexistence metadata");
            }
            else
            {
                require(std::none_of(probe.events.begin(), probe.events.end(), [&](const Event &event) {
                    return event.operation == "create" && event.path == target;
                }), "A failed journal barrier still attempted table creation");
            }
            database.executeSql("CREATE TABLE items (id INT);");
            database.executeSql("INSERT INTO items VALUES (1);");
            require(database.selectAllRows("items").size() == 1,
                    "Failed CREATE prevented the next statement");
        }
    }

    void testInitializationFailure()
    {
        TestDirectory directory{"db-create-initialization"};
        const Path root = directory.path / "db";
        auto engine = StorageEngine::create(root);
        engine.beginStatement();
        // Header serialization throws after the file has been created.
        requireError([&] { engine.createTable("items", std::string(PAGE_SIZE * 2, 'x'), {}, {}); }, "bounds");
        engine.rollbackStatement();
        require(!engine.tableExists("items"), "Header construction failure left an untracked file");
    }

    void testRetryAfterDeletion()
    {
        TestDirectory directory{"db-create-rollback-retry"};
        const Path root = directory.path / "db";
        auto engine = StorageEngine::create(root);
        engine.beginStatement();
        { auto table = engine.createTable("items", "test", {}, {}); }
        Probe probe{};
        probe.root = root;
        probe.target = root / "tables/items.table";
        probe.failOperation = "sync";
        probe.failPath = root / "tables";
        {
            Observe observe{probe};
            requireError([&] { engine.rollbackStatement(); }, "fsync");
        }
        require(probe.failed && !engine.tableExists("items"), "Rollback did not remove the table before failing");
        require(std::filesystem::exists(root / "journal/journal.log"), "Failed rollback lost its journal");
        requireError([&] { engine.commitStatement(); }, "failed");
        engine.rollbackStatement(); // Already absent must be accepted.
        require(!std::filesystem::exists(root / "journal/journal.log"), "Retry did not finish rollback");
        engine.beginStatement();
        engine.commitStatement();
    }

    void testReuseAcrossStatements()
    {
        TestDirectory directory{"db-create-reuse"};
        const Path root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE schema_source (id INT);");
        auto engine = StorageEngine::open(root);
        const auto schema = engine.getTableHeader("schema_source");
        std::vector<std::byte> before;
        {
            engine.beginStatement();
            auto table = engine.createTable("items", "test", schema.columns, schema.constraints);
            engine.commitStatement();
            engine.beginStatement();
            table.insertRows(BoundInsert{.tableName = "items", .rows = {Row{{std::int32_t{1}}}}});
            engine.commitStatement();
            before = readFile(root / "tables/items.table");
            engine.beginStatement();
            table.insertRows(BoundInsert{.tableName = "items", .rows = {Row{{std::int32_t{2}}}}});
            JournalFile journal{root / "journal/journal.log", LinuxFile::OpenMode::ReadOnly};
            const auto image = journal.readNext();
            require(image && !std::get<FileBeforeImage>(*image).newFile,
                    "A later statement still considers a committed table new");
            require(journal.readNext().has_value(), "Existing-table mutation omitted page before-images");
        } // Discard cached modified pages before rollback.
        engine.rollbackStatement();
        require(readFile(root / "tables/items.table") == before,
                "Rollback of reused Table did not restore its committed bytes");
        require(database.selectAllRows("items").size() == 1, "Committed table was lost on rollback");
    }

    void testOffsetsAndMissingFiles()
    {
        TestDirectory directory{"db-recovery-offsets"};
        const Path root = directory.path / "db";
        auto engine = StorageEngine::create(root);
        StatementRecovery recovery{root};
        const Path journal = root / "journal/journal.log";
        requireError([&] { recovery.ensureDurable(0); }, "active statement");
        {
            LinuxFile existing{journal, LinuxFile::OpenMode::CreateNew};
            requireError([&] { recovery.begin(); }, "open");
            require(!recovery.isActive(), "Failed begin left active state");
        }
        std::filesystem::remove(journal);
        requireError([&] { engine.createTable("no_statement", "test", {}, {}); }, "active statement");
        require(!engine.tableExists("no_statement"), "Creation without a statement touched disk");
        for (int statement = 0; statement < 3; ++statement)
        {
            recovery.begin();
            recovery.captureFileOnce("tables/never_created.table", true);
            const auto end = std::filesystem::file_size(journal);
            Probe probe{};
            probe.root = root;
            probe.target = root / "tables/never_created.table";
            {
                Observe observe{probe};
                recovery.ensureDurable(end);
                recovery.ensureDurable(end);
            }
            require(std::count_if(probe.events.begin(), probe.events.end(), [&](const Event &event) {
                return event.operation == "sync" && event.path == journal;
            }) == 1, "Durable offsets were not reset or an already-covered record was resynchronized");
            requireError([&] { recovery.ensureDurable(end + 1); }, "not been appended");
            if (statement == 0)
                recovery.commit();
            else
            {
                recovery.statementFailed();
                requireError([&] { recovery.ensureDurable(end); }, "failed");
                recovery.rollback(); // Recorded file was never created.
            }
        }
        recovery.begin();
        recovery.ensureDurable(); // Empty statement after a nonempty one.
        recovery.commit();

        recovery.begin();
        {
            LinuxFile changed{journal};
            changed.resize(1);
        }
        requireError([&] { recovery.ensureDurable(); }, "does not match completed appends");
        requireError([&] { recovery.commit(); }, "failed");
        require(std::filesystem::exists(journal), "Invariant failure discarded recovery evidence");
    }
}

extern "C"
{
    int __real_open(const char *, int, ...);
    ssize_t __real_pwrite(int, const void *, size_t, off_t);
    int __real_fsync(int);

    int __wrap_open(const char *path, int flags, ...)
    {
        if (flags & O_CREAT)
        {
            va_list args;
            va_start(args, flags);
            const int mode = va_arg(args, int);
            va_end(args);
            if (observe("create", path))
                return -1;
            return __real_open(path, flags, mode);
        }
        return __real_open(path, flags);
    }

    ssize_t __wrap_pwrite(int fd, const void *bytes, size_t count, off_t offset)
    {
        if (observe("write", descriptorPath(fd)))
            return -1;
        return __real_pwrite(fd, bytes, count, offset);
    }

    int __wrap_fsync(int fd)
    {
        if (observe("sync", descriptorPath(fd)))
            return -1;
        return __real_fsync(fd);
    }
}

int main()
{
    try
    {
        testCreationFailures();
        testInitializationFailure();
        testRetryAfterDeletion();
        testReuseAcrossStatements();
        testOffsetsAndMissingFiles();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
