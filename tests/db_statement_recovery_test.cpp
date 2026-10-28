#include "db_test_journal.h"
#include "db_database.h"
#include "db_journal_file.h"
#include "db_statement_recovery.h"
#include "db_storage_engine.h"
#include "db_test_directory.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
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
        off_t offset;
    };

    struct Probe
    {
        Path root;
        Path target;
        std::string failOperation;
        Path failPath;
        unsigned skipMatches = 0;
        bool failed = false;
        bool shortWrite = false;
        bool shortWriteDone = false;
        std::size_t shortWriteBytes = 128;
        std::vector<Event> events;
    };

    Probe *activeProbe = nullptr;

    struct Observe
    {
        explicit Observe(Probe &probe) { activeProbe = &probe; }
        ~Observe() { activeProbe = nullptr; }
    };

    bool observe(const std::string &operation, const Path &path, off_t offset = -1)
    {
        if (!activeProbe)
            return false;
        auto &probe = *activeProbe;
        probe.events.push_back(Event{operation, path,
            std::filesystem::exists(probe.target),
            std::filesystem::exists(probe.root / "journal/journal.log"), offset});
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
        LinuxFile file{LinuxDirectory{path.parent_path()}, path.filename(), LinuxFile::OpenMode::ReadOnly};
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
        for (const std::string boundary : {"journal-sync", "create", "write", "table-sync"})
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
            probe.failPath = boundary == "journal-sync" ? journal : target;
            // Initialization syncs the staging file; journal.log's first sync
            // is the metadata barrier. The write fault follows one page write.
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
            require(database.executeSql("SELECT * FROM items;").rows.size() == 1,
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

    void testBeginBarrierFailures()
    {
        for (const unsigned phase : {0u, 1u})
        {
            TestDirectory directory{"db-begin-barrier"};
            const Path root = directory.path / "db";
            const Path journal = root / "journal/journal.log";
            {
                auto database = Database::create(root, "test");
                Probe probe{};
                probe.root = root;
                probe.target = root / "tables/items.table";
                probe.failOperation = "sync";
                probe.failPath = phase == 1 ? root / "journal" : root / "journal/journal.pending";
                probe.skipMatches = 0;
                {
                    Observe observe{probe};
                    requireError([&] { database.executeSql("CREATE TABLE items (id INT);"); }, "fsync");
                }
                require(probe.failed && !std::filesystem::exists(probe.target),
                        "Failed header barrier allowed a table write");
                require(std::filesystem::file_size(phase == 0 ? root / "journal/journal.pending" : journal) == JournalDataOffset,
                        "Failed begin did not retain the header-only journal");
                requireError([&] { database.executeSql("SELECT * FROM items;"); }, "recovery is required");
            }
            auto reopened = Database::open(root, "test");
            require(!std::filesystem::exists(journal), "Header-only recovery failed to retire the journal");
            reopened.executeSql("CREATE TABLE items (id INT);");
        }
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
        require(probe.failed && !std::filesystem::exists(probe.target), "Rollback did not remove the table before failing");
        requireError([&] { engine.tableExists("items"); }, "recovery is required");
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
        {
            auto database = Database::create(root, "test");
            database.executeSql("CREATE TABLE schema_source (id INT);");
        }
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
            JournalFile journal{LinuxDirectory{root / "journal"}, "journal.log", journal_test::DatabaseIdentity{root}.bytes, journal_test::StatementId, LinuxFile::OpenMode::ReadOnly};
            uuid_t recoveredId{};
            (void)journal.validateJournalHeader(recoveredId);
            const auto image = journal.readNext();
            require(image && !std::get<FileBeforeImage>(image->record).newFile,
                    "A later statement still considers a committed table new");
            require(journal.readNext().has_value(), "Existing-table mutation omitted page before-images");
        } // Discard cached modified pages before rollback.
        engine.rollbackStatement();
        require(readFile(root / "tables/items.table") == before,
                "Rollback of reused Table did not restore its committed bytes");
        auto restored = engine.openTable("items");
        auto cursor = restored.scan();
        require(cursor.next().has_value() && !cursor.next(), "Committed table was lost on rollback");
    }

    void testOffsetsAndMissingFiles()
    {
        TestDirectory directory{"db-recovery-offsets"};
        const Path root = directory.path / "db";
        auto engine = StorageEngine::create(root);
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory{root}, identity.bytes};
        const Path journal = root / "journal/journal.log";
        requireError([&] { recovery.ensureDurable(0); }, "active statement");
        {
            LinuxFile existing{LinuxDirectory{journal.parent_path()}, journal.filename(), LinuxFile::OpenMode::CreateNew};
            requireError([&] { recovery.begin(); }, "open");
            require(!recovery.isActive(), "Failed begin left active state");
        }
        std::filesystem::remove(journal);
        requireError([&] { engine.createTable("no_statement", "test", {}, {}); }, "active statement");
        require(!engine.tableExists("no_statement"), "Creation without a statement touched disk");
        for (int statement = 0; statement < 3; ++statement)
        {
            recovery.begin();
            recovery.captureFileOnce("never_created.table", true);
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
            }) == 2, "Durable offsets were not reset or an already-covered record was resynchronized");
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
            LinuxFile changed{LinuxDirectory{journal.parent_path()}, journal.filename()};
            changed.resize(1);
        }
        requireError([&] { recovery.ensureDurable(); }, "does not match completed appends");
        requireError([&] { recovery.commit(); }, "failed");
        require(std::filesystem::exists(journal), "Invariant failure discarded recovery evidence");
    }
    void seedTables(const Path &root)
    {
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE items (id INT, label TEXT);");
        database.executeSql("CREATE TABLE other (id INT, label TEXT);");
        database.executeSql("INSERT INTO items VALUES (1, '" + std::string(1500, 'a') + "');");
        database.executeSql("INSERT INTO other VALUES (1, '" + std::string(1500, 'a') + "');");
    }

    std::size_t eventCount(const Probe &probe, const std::string &operation, const Path &path)
    {
        return std::count_if(probe.events.begin(), probe.events.end(), [&](const Event &event) {
            return event.operation == operation && event.path == path;
        });
    }

    void testCoverageGenerationsAndBatches()
    {
        TestDirectory directory{"db-coverage"};
        const Path root = directory.path / "db";
        seedTables(root);
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory{root}, identity.bytes};
        const Path first = "items.table";
        const Path second = "other.table";
        const Path journal = root / "journal/journal.log";
        BufferManager a{LinuxDirectory{root / "tables"}, first.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
        BufferManager b{LinuxDirectory{root / "tables"}, second.filename(), LinuxFile::OpenMode::OpenExisting, recovery};
        recovery.begin();
        a.getHeaderPage().write<HeaderPage>().magic = "changed";
        const auto firstCoverage = recovery.requiredCoverageForPage(first, 0);
        Probe probe{};
        probe.root = root;
        probe.target = root / "tables" / first;
        {
            Observe observe{probe};
            a.flushPage(0);
            // Append more undo, then rewrite an already-covered page. The new
            // records need not be synced until their dependent write is issued.
            b.getHeaderPage().write<HeaderPage>().magic = "other";
            a.getHeaderPage().write<HeaderPage>().magic = "again";
            a.flushPage(0);
            require(eventCount(probe, "sync", journal) == 2,
                    "Already-covered single-page write repeated the barrier");
            b.getDataPage(1).write<DataPage>().rows[0].row.values[0] = std::int32_t{2};
            b.flushAll();
            require(eventCount(probe, "sync", journal) == 4,
                    "A batch must use exactly one two-sync publication");
            require(eventIndex(probe, "sync", journal) < eventIndex(probe, "write", root / "tables" / first),
                    "Existing-page write preceded its journal barrier");
            a.sync();
            b.sync();
        }
        const auto secondCoverage = recovery.requiredCoverageForPage(second, 0);
        require(firstCoverage.end != secondCoverage.end,
                "Equal page IDs in different files shared a before-image");
        requireError([&] { recovery.validateCoverage(first, 0, secondCoverage); }, "does not match");
        recovery.commit();

        // The same before-image layout produces the same offset, but a new
        // generation. A numeric comparison alone would accept the stale token.
        recovery.begin();
        a.getHeaderPage().write<HeaderPage>().magic = "next";
        const auto nextCoverage = recovery.requiredCoverageForPage(first, 0);
        require(firstCoverage.end == nextCoverage.end && firstCoverage.generation != nextCoverage.generation,
                "Coverage did not distinguish identical offsets across statements");
        requireError([&] { recovery.ensureDurable(firstCoverage); }, "generation");
        requireError([&] { recovery.validateCoverage(first, 0, firstCoverage); }, "generation");
        a.flushPage(0);
        a.sync();
        recovery.rollback();
        requireError([&] { a.getHeaderPage(); }, "invalidated by rollback");
        recovery.begin();
        recovery.captureFileOnce(first);
        recovery.capturePageOnce({first, 0, PageFile{LinuxDirectory{root / "tables"}, first}.readPage(0)});
        requireError([&] { recovery.ensureDurable(nextCoverage); }, "generation");
        recovery.rollback();
    }

    void testBoundaryPublicationOrder()
    {
        TestDirectory directory{"db-boundary-publication"};
        const Path root = directory.path / "db";
        auto engine = StorageEngine::create(root);
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory{root}, identity.bytes};
        recovery.begin();
        const Path path = root / "journal/journal.log";
        JournalFile reader{LinuxDirectory{root / "journal"}, "journal.log", identity.bytes,
                           journal_test::StatementId, LinuxFile::OpenMode::ReadOnly};
        uuid_t recovered{};
        (void)reader.validateJournalHeader(recovered);
        auto a = reader.readJournalBoundary(0).value();
        auto b = reader.readJournalBoundary(1).value();
        unsigned active = a.generation >= b.generation ? 0 : 1;
        auto generation = std::max(a.generation, b.generation);
        for (const std::string file : {"first.table", "second.table", "third.table"})
        {
            recovery.captureFileOnce(file, true);
            const auto end = reader.size();
            const auto before = readFile(path);
            Probe probe{};
            probe.root = root;
            probe.target = root / "tables" / file;
            {
                Observe observe{probe};
                recovery.ensureDurable(end);
                recovery.ensureDurable(end); // Already-covered requests do no I/O.
            }
            require(probe.events.size() == 3 &&
                        probe.events[0].operation == "sync" && probe.events[0].path == path &&
                        probe.events[1].operation == "write" && probe.events[1].path == path &&
                        probe.events[1].offset == static_cast<off_t>(journalBoundaryOffset(static_cast<std::uint8_t>(1 - active))) &&
                        probe.events[2].operation == "sync" && probe.events[2].path == path,
                    "Publication must sync records, overwrite the older slot, then sync the boundary");
            const auto current = reader.readJournalBoundary(static_cast<std::uint8_t>(1 - active)).value();
            const auto previous = reader.readJournalBoundary(static_cast<std::uint8_t>(active)).value();
            require(current.generation == generation + 1 && current.endOffset == end && previous.generation == generation,
                    "Publication did not preserve the previous generation");
            const auto after = readFile(path);
            require(before.size() == after.size() &&
                        std::equal(before.begin(), before.begin() + JournalHeaderBytes, after.begin()) &&
                        std::equal(before.begin() + JournalDataOffset, before.end(), after.begin() + JournalDataOffset),
                    "Boundary publication changed the immutable header or records");
            active = 1 - active;
            ++generation;
        }
        recovery.rollback();
    }

    void testPublicationFailuresBlockDatabaseWrites()
    {
        for (const std::string phase : {"record-sync", "boundary-write", "boundary-sync"})
        {
            TestDirectory directory{"db-publication-failure"};
            const Path root = directory.path / "db";
            seedTables(root);
            const Path target = root / "tables/items.table";
            const Path journal = root / "journal/journal.log";
            const auto before = readFile(target);
            const journal_test::DatabaseIdentity identity{root};
            StatementRecovery recovery{LinuxDirectory{root}, identity.bytes};
            BufferManager manager{LinuxDirectory{root / "tables"}, "items.table", LinuxFile::OpenMode::OpenExisting, recovery};
            recovery.begin();
            manager.getHeaderPage().write<HeaderPage>().magic = "changed";
            Probe probe{};
            probe.root = root;
            probe.target = target;
            probe.failPath = journal;
            probe.failOperation = phase == "boundary-write" ? "write" : "sync";
            probe.skipMatches = phase == "boundary-sync" ? 1 : 0;
            {
                Observe observe{probe};
                requireError([&] { manager.flushPage(0); }, "Input/output error");
            }
            require(probe.failed && eventCount(probe, "write", target) == 0,
                    "Failed publication allowed a dependent database write: " + phase);
            require(readFile(target) == before && std::filesystem::exists(journal),
                    "Failed publication changed data or discarded undo");
            requireError([&] { recovery.commit(); }, "failed");
            recovery.rollback();
            require(readFile(target) == before && !std::filesystem::exists(journal),
                    "Failed publication could not be rolled back");
        }
    }

    void testCommitChecksEveryHandle()
    {
        for (const bool flush : {false, true})
        {
            TestDirectory directory{"db-commit-participants"};
            const Path root = directory.path / "db";
            seedTables(root);
            const auto before = readFile(root / "tables/items.table");
            const journal_test::DatabaseIdentity identity{root};
            StatementRecovery recovery{LinuxDirectory{root}, identity.bytes};
            recovery.begin();
            {
                BufferManager writer{LinuxDirectory{root / "tables"}, "items.table", LinuxFile::OpenMode::OpenExisting, recovery};
                writer.getHeaderPage().write<HeaderPage>().magic = "changed";
                if (flush)
                    writer.flushAll();
                // Synchronizing a different handle/file must not acknowledge
                // this writer, nor may destroying its cache hide pending work.
                BufferManager other{LinuxDirectory{root / "tables"}, "other.table", LinuxFile::OpenMode::OpenExisting, recovery};
                other.getHeaderPage().write<HeaderPage>().magic = "other";
                other.flushAll();
                other.sync();
                BufferManager samePath{LinuxDirectory{root / "tables"}, "items.table", LinuxFile::OpenMode::OpenExisting, recovery};
                samePath.sync();
            }
            requireError([&] { recovery.commit(); }, flush ? "unsynchronized" : "unflushed");
            require(std::filesystem::exists(root / "journal/journal.log"), "Unsafe commit retired undo");
            recovery.rollback();
            require(readFile(root / "tables/items.table") == before, "Participant rollback lost original bytes");
        }
    }

    void testExistingWriteFailuresAndGrowth()
    {
        for (const std::string boundary : {"journal-sync", "single-write", "partial-write", "batch-write", "table-sync"})
        {
            TestDirectory directory{"db-existing-write-failure"};
            const Path root = directory.path / "db";
            seedTables(root);
            const Path target = root / "tables/items.table";
            const Path journal = root / "journal/journal.log";
            const auto before = readFile(target);
            const journal_test::DatabaseIdentity identity{root};
            StatementRecovery recovery{LinuxDirectory{root}, identity.bytes};
            BufferManager manager{LinuxDirectory{root / "tables"}, "items.table", LinuxFile::OpenMode::OpenExisting, recovery};
            auto guard = manager.getHeaderPage();
            auto table = Table::open(LinuxDirectory{root / "tables"}, "items.table", recovery);
            auto cursor = table.scan();
            require(cursor.next().has_value(), "Cursor fixture is empty");
            recovery.begin();
            // Force file growth and changes to existing page links and header.
            manager.insertAllRows({Row{{std::int32_t{2}, std::string(1500, 'b')}},
                                   Row{{std::int32_t{3}, std::string(1500, 'c')}}});
            Probe probe{};
            probe.root = root;
            probe.target = target;
            probe.failOperation = boundary == "single-write" || boundary == "partial-write" || boundary == "batch-write" ? "write" : "sync";
            probe.failPath = boundary == "journal-sync" ? journal : target;
            probe.shortWrite = boundary == "partial-write";
            probe.skipMatches = boundary == "batch-write" || probe.shortWrite ? 1 : 0;
            {
                Observe observe{probe};
                requireError([&] {
                    if (boundary == "single-write" || boundary == "partial-write")
                        manager.flushPage(0);
                    else
                        manager.flushAll();
                    manager.sync();
                }, "Input/output error");
                requireError([&] { guard.page(); }, "recovery is required");
                requireError([&] { manager.getHeaderPage(); }, "recovery is required");
                requireError([&] { cursor.next(); }, "recovery is required");
                requireError([&] { table.scan(); }, "recovery is required");
                requireError([&] { table.insertRows(BoundInsert{.tableName = "items", .rows = {}}); }, "recovery is required");
                requireError([&] { manager.flushAll(); }, "recovery is required");
                requireError([&] { manager.flushPage(0); }, "recovery is required");
                requireError([&] { manager.sync(); }, "recovery is required");
                requireError([&] { recovery.ensureDurable(0); }, "recovery is required");
                requireError([&] { recovery.commit(); }, "recovery is required");
            }
            require(probe.failed, "Failure boundary was not exercised: " + boundary);
            if (boundary == "partial-write")
                require(probe.shortWriteDone && readFile(target) != before, "Short write did not modify a prefix");
            if (boundary == "table-sync")
                require(std::filesystem::file_size(target) > before.size(), "Growth fixture did not extend the file");
            if (boundary == "journal-sync")
                require(eventCount(probe, "write", target) == 0, "Failed barrier allowed a table write");
            require(std::filesystem::exists(journal), "I/O failure discarded journal");
            recovery.rollback();
            require(readFile(target) == before, "Rollback did not restore grown file, links, and header");
            requireError([&] { guard.page(); }, "invalidated by rollback");
            requireError([&] { cursor.next(); }, "invalidated by rollback");
            auto restored = Table::open(LinuxDirectory{root / "tables"}, "items.table", recovery);
            auto scan = restored.scan();
            require(scan.next().has_value() && !scan.next(), "Fresh handle did not see restored contents");
        }
    }

    void testMultipleFileFailureAndReadGating()
    {
        TestDirectory directory{"db-multiple-file-failure"};
        const Path root = directory.path / "db";
        seedTables(root);
        auto engine = StorageEngine::open(root);
        const Path first = root / "tables/items.table";
        const Path second = root / "tables/other.table";
        const auto firstBefore = readFile(first);
        const auto secondBefore = readFile(second);
        auto a = engine.openTable("items");
        auto b = engine.openTable("other");
        engine.beginStatement();
        a.insertRows({.tableName = "items", .rows = {Row{{std::int32_t{2}, std::string(1500, 'b')}}}});
        Probe probe{};
        probe.root = root;
        probe.target = second;
        probe.failOperation = "sync";
        probe.failPath = second;
        {
            Observe observe{probe};
            requireError([&] {
                b.insertRows({.tableName = "other", .rows = {Row{{std::int32_t{2}, std::string(1500, 'b')}}}});
            }, "fsync");
        }
        requireError([&] { engine.tableExists("items"); }, "recovery is required");
        requireError([&] { engine.getTableHeader("items"); }, "recovery is required");
        requireError([&] { engine.openTable("items"); }, "recovery is required");
        requireError([&] { a.scan(); }, "recovery is required");
        requireError([&] { engine.commitStatement(); }, "recovery is required");
        // Recovery itself fails after restoring some pages. Access stays blocked
        // and another rollback must be able to replay the retained journal.
        probe.failed = false;
        {
            Observe observe{probe};
            requireError([&] { engine.rollbackStatement(); }, "fsync");
        }
        requireError([&] { engine.getTableHeader("items"); }, "recovery is required");
        require(std::filesystem::exists(root / "journal/journal.log"), "Failed recovery lost undo");
        engine.rollbackStatement();
        require(readFile(first) == firstBefore && readFile(second) == secondBefore,
                "Multi-file rollback retained part of the failed statement");
        requireError([&] { a.scan(); }, "invalidated by rollback");
        require(engine.getTableHeader("items").totalRowCount == 1, "Catalog did not reopen after recovery");
        engine.beginStatement();
        engine.commitStatement();
    }

    void testAppendFailureAfterWriteback()
    {
        TestDirectory directory{"db-append-failure"};
        const Path root = directory.path / "db";
        seedTables(root);
        const Path target = root / "tables/items.table";
        const auto before = readFile(target);
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory{root}, identity.bytes};
        BufferManager manager{LinuxDirectory{root / "tables"}, "items.table", LinuxFile::OpenMode::OpenExisting, recovery};
        recovery.begin();
        manager.getHeaderPage().write<HeaderPage>().magic = "changed";
        manager.flushPage(0);
        manager.sync();
        Probe probe{};
        probe.root = root;
        probe.target = target;
        probe.failOperation = "write";
        probe.failPath = root / "journal/journal.log";
        probe.shortWrite = true;
        probe.skipMatches = 1;
        {
            Observe observe{probe};
            requireError([&] { manager.getDataPage(1).write<DataPage>(); }, "pwrite");
            requireError([&] { recovery.commit(); }, "recovery is required");
            requireError([&] { manager.getHeaderPage(); }, "recovery is required");
        }
        require(probe.shortWriteDone && probe.failed, "Journal append did not fail after a partial transfer");
        require(std::filesystem::exists(probe.failPath), "Failed capture lost the journal");
        recovery.rollback(); // Replay the known complete prefix, including earlier writeback.
        require(readFile(target) == before, "Append failure prevented undo of earlier database writes");
    }

    void testRetirementFailures()
    {
        for (const bool commit : {false, true})
        {
            TestDirectory directory{"db-retirement-failure"};
            const Path root = directory.path / "db";
            seedTables(root);
            auto engine = StorageEngine::open(root);
            const Path target = root / "tables/items.table";
            const auto before = readFile(target);
            engine.beginStatement();
            auto table = engine.openTable("items");
            table.insertRows({.tableName = "items", .rows = {Row{{std::int32_t{2}, std::string{"b"}}}}});
            Probe probe{};
            probe.root = root;
            probe.target = target;
            probe.failOperation = "sync";
            probe.failPath = root / "journal";
            {
                Observe observe{probe};
                requireError([&] {
                    if (commit)
                        engine.commitStatement();
                    else
                        engine.rollbackStatement();
                }, "fsync");
            }
            require(probe.failed && !std::filesystem::exists(root / "journal/journal.log"),
                    "Retirement failure did not follow unlink");
            requireError([&] { engine.getTableHeader("items"); }, "recovery is required");
            requireError([&] { table.scan(); }, "recovery is required");
            if (commit)
            {
                const auto committed = readFile(target);
                requireError([&] { engine.rollbackStatement(); }, "Commit outcome is uncertain");
                require(readFile(target) == committed, "Uncertain commit was replayed as rollback");
            }
            else
            {
                engine.rollbackStatement(); // Retry directory cleanup without replaying undo.
                require(readFile(target) == before, "Rollback cleanup retry changed restored data");
                requireError([&] { table.scan(); }, "invalidated by rollback");
                require(engine.getTableHeader("items").totalRowCount == 1,
                        "Cleanup retry did not restore normal catalog access");
            }
        }
    }

    void testFinalCleanupReadGating()
    {
        TestDirectory directory{"db-commit-cleanup"};
        const Path root = directory.path / "db";
        seedTables(root);
        {
            auto database = Database::open(root, "test");
            Probe probe{};
            probe.root = root;
            probe.target = root / "tables/items.table";
            probe.failOperation = "sync";
            probe.failPath = root / "journal";
            probe.skipMatches = 1; // First sync persists journal creation.
            {
                Observe observe{probe};
                requireError([&] { database.executeSql("INSERT INTO items VALUES (2, 'b');"); }, "fsync");
            }
            require(probe.failed && !std::filesystem::exists(root / "journal/journal.log"),
                    "Did not fail after journal unlink");
            requireError([&] { database.executeSql("SELECT * FROM items;"); }, "recovery is required");
            requireError([&] { database.executeSql("INSERT INTO items VALUES (3, 'blocked');"); }, "recovery is required");
            requireError([&] { database.executeSql("DELETE FROM items;"); }, "recovery is required");
            requireError([&] { database.executeSql("CREATE TABLE blocked (id INT);"); }, "recovery is required");
        }
        // In-process failure requires reopening; this is not a power-loss test.
        auto database = Database::open(root, "test");
        require(database.executeSql("SELECT * FROM items;").rows.size() == 2, "Uncertain commit was blindly undone");
    }

    void testDirectDirectorySyncFailures()
    {
        for (const bool tables : {false, true})
        {
            TestDirectory directory{"db-directory-sync-failure"};
            const Path root = directory.path / "db";
            auto engine = StorageEngine::create(root);
            engine.beginStatement();
            Probe probe{};
            probe.root = root;
            probe.target = root / "tables/items.table";
            probe.failOperation = "sync";
            probe.failPath = root / (tables ? "tables" : "journal");
            {
                Observe observe{probe};
                requireError([&] {
                    if (tables)
                        engine.syncTablesDirectory();
                    else
                        engine.syncJournalDirectory();
                }, "fsync");
            }
            require(probe.failed, "Directory sync fault was not reached");
            requireError([&] { engine.tableExists("items"); }, "recovery is required");
            requireError([&] { engine.commitStatement(); }, "recovery is required");
            engine.rollbackStatement();
            require(!engine.tableExists("items"), "Empty statement rollback did not restore access");
        }
    }

    void testRestartableRecoveryFailures()
    {
        for (const bool reopen : {false, true})
        for (const std::string phase : {"write", "short-write", "resize", "file-sync",
                                       "table-unlink", "table-sync", "journal-unlink", "journal-sync"})
        {
            TestDirectory directory{"db-replay-retry"};
            const Path root = directory.path / "db";
            seedTables(root);
            const auto before = readFile(root / "tables/items.table");
            {
                auto engine = StorageEngine::open(root);
                auto table = engine.openTable("items");
                engine.beginStatement();
                table.insertRows({.tableName = "items", .rows = {
                    Row{{std::int32_t{2}, std::string(1500, 'b')}},
                    Row{{std::int32_t{3}, std::string(1500, 'c')}}}});
                { auto added = engine.createTable("added", "test", {}, {}); }
                require(readFile(root / "tables/items.table").size() > before.size(), "Retry fixture did not grow");
                Probe probe{};
                probe.root = root;
                probe.target = root / "tables/items.table";
                probe.failPath = probe.target;
                probe.failOperation = phase;
                if (phase == "short-write")
                {
                    probe.failOperation = "write";
                    probe.shortWrite = true;
                    probe.skipMatches = 1;
                }
                if (phase == "file-sync") probe.failOperation = "sync";
                if (phase == "table-unlink" || phase == "journal-unlink")
                {
                    probe.failOperation = "unlink";
                    probe.failPath = root / (phase == "table-unlink" ? "tables/added.table" : "journal/journal.log");
                }
                if (phase == "table-sync" || phase == "journal-sync")
                {
                    probe.failOperation = "sync";
                    probe.failPath = root / (phase == "table-sync" ? "tables" : "journal");
                }
                {
                    Observe observe{probe};
                    requireError([&] { engine.rollbackStatement(); }, "Input/output error");
                }
                require(probe.failed, "Recovery failure not reached: " + phase);
                requireError([&] { engine.getTableHeader("items"); }, "recovery is required");
                requireError([&] { table.scan(); }, "recovery is required");
                if (phase != "journal-sync")
                    require(std::filesystem::exists(root / "journal/journal.log"), "Recovery failure lost undo");
                if (!reopen) engine.rollbackStatement();
            }
            auto engine = StorageEngine::open(root);
            require(readFile(root / "tables/items.table") == before, "Recovery retry lost original bytes: " + phase);
            require(!engine.tableExists("added"), "Recovery retry retained a new table");
            engine.beginStatement();
            engine.commitStatement();
        }
    }

    void testPartialInitialization()
    {
        for (const bool reopen : {false, true})
        for (const std::string phase : {"header", "slot-a", "slot-b", "rename"})
        {
            TestDirectory directory{"db-partial-header"};
            const Path root = directory.path / "db";
            {
                auto engine = StorageEngine::create(root);
                Probe probe{};
                probe.root = root;
                probe.target = root / "tables/items.table";
                probe.failPath = root / "journal/journal.pending";
                probe.failOperation = phase == "rename" ? "rename" : "write";
                probe.shortWrite = phase == "header";
                probe.shortWriteBytes = 16; // Tear inside the serialized header itself.
                probe.skipMatches = phase == "header" || phase == "slot-a" ? 1 : phase == "slot-b" ? 2 : 0;
                {
                    Observe observe{probe};
                    requireError([&] { engine.beginStatement(); }, "Input/output error");
                }
                require(probe.failed, "Initialization failure not reached");
                requireError([&] { engine.tableExists("items"); }, "recovery is required");
                if (!reopen) engine.rollbackStatement();
            }
            auto engine = StorageEngine::open(root);
            require(!std::filesystem::exists(root / "journal/journal.pending"), "Staging journal survived recovery");
            engine.beginStatement();
            engine.commitStatement();
        }
    }
}

extern "C"
{
    long __real_syscall(long, ...);

    long __wrap_syscall(long number, ...)
    {
        if (number != SYS_openat2) { errno = ENOSYS; return -1; }
        va_list args;
        va_start(args, number);
        const int parent = va_arg(args, int);
        const char *path = va_arg(args, const char *);
        const auto *how = va_arg(args, struct open_how *);
        const auto size = va_arg(args, size_t);
        va_end(args);
        if ((how->flags & O_CREAT) && observe("create", descriptorPath(parent) / path))
            return -1;
        return __real_syscall(number, parent, path, how, size);
    }

    int __real_open(const char *, int, ...);
    ssize_t __real_pwrite(int, const void *, size_t, off_t);
    int __real_fsync(int);
    int __real_ftruncate(int, off_t);
    int __real_unlinkat(int, const char *, int);
    int __real_renameat(int, const char *, int, const char *);

    int __wrap_ftruncate(int fd, off_t size)
    {
        if (observe("resize", descriptorPath(fd))) return -1;
        return __real_ftruncate(fd, size);
    }

    int __wrap_unlinkat(int fd, const char *path, int flags)
    {
        if (observe("unlink", descriptorPath(fd) / path)) return -1;
        return __real_unlinkat(fd, path, flags);
    }

    int __wrap_renameat(int from, const char *source, int to, const char *target)
    {
        if (observe("rename", descriptorPath(from) / source)) return -1;
        return __real_renameat(from, source, to, target);
    }

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
        const auto path = descriptorPath(fd);
        if (observe("write", path, offset))
            return -1;
        if (activeProbe && activeProbe->shortWrite && !activeProbe->shortWriteDone &&
            path == activeProbe->failPath)
        {
            activeProbe->shortWriteDone = true;
            return __real_pwrite(fd, bytes, std::min(count, activeProbe->shortWriteBytes), offset);
        }
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
        testBeginBarrierFailures();
        testRetryAfterDeletion();
        testReuseAcrossStatements();
        testOffsetsAndMissingFiles();
        testCoverageGenerationsAndBatches();
        testBoundaryPublicationOrder();
        testPublicationFailuresBlockDatabaseWrites();
        testCommitChecksEveryHandle();
        testExistingWriteFailuresAndGrowth();
        testMultipleFileFailureAndReadGating();
        testAppendFailureAfterWriteback();
        testRetirementFailures();
        testFinalCleanupReadGating();
        testDirectDirectorySyncFailures();
        testRestartableRecoveryFailures();
        testPartialInitialization();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
