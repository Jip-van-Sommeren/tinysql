#include "db_database.h"
#include "db_linux_file.h"
#include "db_storage_engine.h"
#include "db_test_directory.h"
#include "db_test_journal.h"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
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
        catch (const std::runtime_error &error)
        {
            require(std::string{error.what()}.find(message) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected error: " + message);
    }

    struct WorkingDirectory
    {
        const std::filesystem::path original = std::filesystem::current_path();

        explicit WorkingDirectory(const std::filesystem::path &path)
        {
            std::filesystem::current_path(path);
        }

        ~WorkingDirectory()
        {
            std::error_code error;
            std::filesystem::current_path(original, error);
        }
    };

    std::vector<std::byte> readFile(const std::filesystem::path &path)
    {
        LinuxFile file{LinuxDirectory{path.parent_path()}, path.filename(), LinuxFile::OpenMode::ReadOnly};
        std::vector<std::byte> bytes(static_cast<std::size_t>(file.size()));
        file.readExactAt(0, bytes);
        return bytes;
    }

    void testStorageIntegration()
    {
        TestDirectory directory{"db-storage-integration-test"};
        const auto root = directory.path / "db";
        std::optional<Database> database{Database::create(root, "test")};

        const auto unrelated = directory.path / "unrelated";
        std::filesystem::create_directories(unrelated / "tables");
        {
            LinuxFile decoy{LinuxDirectory{(unrelated / "tables/items.table").parent_path()}, (unrelated / "tables/items.table").filename(), LinuxFile::OpenMode::CreateNew};
            LinuxFile missing{LinuxDirectory{(unrelated / "tables/missing.table").parent_path()}, (unrelated / "tables/missing.table").filename(), LinuxFile::OpenMode::CreateNew};
        }
        WorkingDirectory workingDirectory{unrelated};

        // An unrelated file with the same relative name must not block creation.
        database->executeSql("CREATE TABLE items (id INT NOT NULL);");
        const auto tablePath = root / "tables/items.table";
        const auto journalPath = root / "journal/journal.log";
        require(std::filesystem::exists(tablePath), "Table was not created in the database root");

        database.reset(); // Release exclusive ownership before reopening.
        {
            StorageEngine engine = StorageEngine::open(root);
            require(engine.tableExists("items"), "Catalog did not resolve the database root");
            requireError([&] { engine.createTable("items", "test", {}, {}); }, "Table already exists");
            requireError([&] { engine.openTable("missing"); }, "Table does not exist");
            require(!std::filesystem::exists(root / "tables/missing.table"),
                    "Opening a missing table created it");
        }

        database.emplace(Database::open(root, "test"));
        database->insertRows("items", {Row{{std::int32_t{1}}}});
        require(!std::filesystem::exists(journalPath), "Successful direct INSERT left a journal");
        const auto before = readFile(tablePath);

        // The first row is valid and modifies cached pages before the second
        // row fails validation. The direct API must take the rollback path.
        requireError([&]
        {
            database->insertRows("items", {
                Row{{std::int32_t{2}}}, Row{{std::string{"not an integer"}}}});
        }, "expects type");
        require(readFile(tablePath) == before, "Failed direct INSERT changed the table bytes");
        require(!std::filesystem::exists(journalPath), "Rollback left an active journal");
        require(database->selectAllRows("items").size() == 1, "Failed INSERT retained a row");

        requireError([&] { database->insertRows("missing", {Row{{std::int32_t{3}}}}); },
                     "Table does not exist");
        require(!std::filesystem::exists(journalPath), "Failed table lookup left an active journal");

        database->insertRows("items", {});
        require(readFile(tablePath) == before && !std::filesystem::exists(journalPath),
                "Empty direct INSERT changed data or left a journal");
        database->insertRows("items", {Row{{std::int32_t{2}}}});

        database.reset();
        Database reopened = Database::open(root, "test");
        const auto rows = reopened.selectAllRows("items");
        require(rows.size() == 2 && std::get<std::int32_t>(rows[0].values.at(0)) == 1 &&
                    std::get<std::int32_t>(rows[1].values.at(0)) == 2,
                "Direct INSERT did not recover for the next statement or survive reopening");
        require(std::filesystem::file_size(unrelated / "tables/items.table") == 0,
                "Database modified the working-directory decoy file");
    }

    void testMetadataValidation()
    {
        TestDirectory directory{"db-meta-validation"};
        const auto root = directory.path / "db";
        { auto engine = StorageEngine::create(root); }
        const auto metaPath = root / "db.meta";
        const auto original = readFile(metaPath);
        require(original.size() == 36, "Database metadata omitted fields or checksum");
        const journal_test::DatabaseIdentity identity{root};
        require(!uuid_is_null(identity.bytes), "Database UUID is empty");
        {
            auto engine = StorageEngine::open(root);
            engine.beginStatement();
            JournalFile journal(LinuxDirectory{root / "journal"}, "journal.log", identity.bytes, journal_test::StatementId, LinuxFile::OpenMode::ReadOnly);
            const auto header = journal.readJournalHeader();
            require(uuid_compare(header.dbId, identity.bytes) == 0 && !uuid_is_null(header.statementId),
                    "Statement journal lost its database or statement identity");
            engine.rollbackStatement();
        }
        const auto reject = [&](journal_test::Bytes bytes, const std::string &message)
        {
            LinuxFile meta(LinuxDirectory{metaPath.parent_path()}, metaPath.filename());
            meta.resize(0);
            meta.writeAllAt(0, bytes);
            requireError([&] { (void)StorageEngine::open(root); }, message);
            require(readFile(metaPath) == bytes, "Failed open changed invalid metadata");
        };
        auto bytes = original;
        bytes[0] ^= std::byte{1};
        reject(bytes, "meta file");
        bytes = original;
        bytes[8] ^= std::byte{1};
        reject(bytes, "checksum");
        bytes = original;
        journal_test::setUnsigned(bytes, 24, std::uint64_t{2});
        journal_test::setUnsigned(bytes, 32, journal_test::crc(std::span<const std::byte>{bytes}.first(32)));
        reject(bytes, "version");
        bytes = original; bytes.pop_back();
        reject(bytes, "EOF");
        LinuxFile meta(LinuxDirectory{metaPath.parent_path()}, metaPath.filename());
        meta.resize(0);
        meta.writeAllAt(0, original);
        { auto engine = StorageEngine::open(root); }
    }

    void testRestartValidationBeforeReplay()
    {
        TestDirectory directory{"db-restart-validation"};
        const auto root = directory.path / "db";
        const auto tablePath = root / "tables/items.table";
        const auto journalPath = root / "journal/journal.log";
        {
            auto database = Database::create(root, "test");
            database.executeSql("CREATE TABLE items (id INT);");
            database.executeSql("INSERT INTO items VALUES (1);");
        }
        const auto before = readFile(tablePath);
        {
            auto engine = StorageEngine::open(root);
            const auto schema = engine.getTableHeader("items");
            engine.beginStatement();
            auto table = engine.openTable("items");
            table.insertRows({.tableName = "items", .rows = {Row{{std::int32_t{2}}}}});
            auto created = engine.createTable("uncommitted", "test", schema.columns, schema.constraints);
        } // Close handles without retiring the durable undo journal.
        const auto changed = readFile(tablePath);
        const auto originalJournal = readFile(journalPath);
        require(changed != before, "Restart fixture did not write modified table bytes");
        const auto reject = [&](const journal_test::Bytes &bytes, const std::string &message)
        {
            LinuxFile journal(LinuxDirectory{journalPath.parent_path()}, journalPath.filename());
            journal.resize(0);
            journal.writeAllAt(0, bytes);
            requireError([&] { (void)StorageEngine::open(root); }, message);
            require(readFile(tablePath) == changed, "Invalid journal partially replayed before validation finished");
            require(std::filesystem::exists(root / "tables/uncommitted.table"), "Invalid journal deleted a table");
            require(readFile(journalPath) == bytes, "Failed recovery discarded or changed its evidence");
        };
        auto bytes = originalJournal;
        bytes[16] ^= std::byte{1}; // A different DB UUID with a valid header CRC.
        journal_test::setUnsigned(bytes, 56, journal_test::crc(std::span<const std::byte>{bytes}.first(56)));
        reject(bytes, "db id");
        bytes = originalJournal; bytes.back() ^= std::byte{1};
        reject(bytes, "checksum");
        bytes = originalJournal; bytes.pop_back();
        reject(bytes, "endoffset");

        bytes = originalJournal; bytes.resize(59);
        reject(bytes, "EOF");
        {
            LinuxFile journal(LinuxDirectory{journalPath.parent_path()}, journalPath.filename());
            journal.resize(0);
            journal.writeAllAt(0, originalJournal);
        }
        auto recovered = StorageEngine::open(root);
        require(readFile(tablePath) == before, "Restart did not restore the original table bytes");
        require(!recovered.tableExists("uncommitted") && !std::filesystem::exists(journalPath),
                "Restart did not remove the new table and retire the journal");
        recovered.beginStatement();
        recovered.commitStatement();
    }

    void testUnpublishedTailsAreIgnored()
    {
        for (const std::string tail : {"partial", "complete", "corrupt"})
        {
            TestDirectory directory{"db-unpublished-tail"};
            const auto root = directory.path / "db";
            const auto target = root / "tables/items.table";
            const auto journalPath = root / "journal/journal.log";
            {
                auto database = Database::create(root, "test");
                database.executeSql("CREATE TABLE items (id INT);");
                database.executeSql("CREATE TABLE victim (id INT);");
                database.executeSql("INSERT INTO items VALUES (1);");
            }
            const auto before = readFile(target);
            {
                auto engine = StorageEngine::open(root);
                engine.beginStatement();
                auto table = engine.openTable("items");
                table.insertRows({.tableName = "items", .rows = {Row{{std::int32_t{2}}}}});
            }
            require(readFile(target) != before, "Tail fixture did not issue a database write");
            const journal_test::DatabaseIdentity db{root};
            LinuxDirectory journalDir{root / "journal"};
            uuid_t statement{};
            JournalFile reader(journalDir, "journal.log", db.bytes, statement, LinuxFile::OpenMode::ReadOnly);
            const auto publishedEnd = reader.validateJournalHeader(statement);
            const auto identity = reader.readJournalHeader();
            std::uint32_t sequence = 0;
            while (reader.readNext(publishedEnd)) ++sequence;
            // This complete, correctly sequenced record would delete a committed
            // table if recovery accidentally included the unpublished suffix.
            auto suffix = journal_test::encode(FileBeforeImage{"victim.table", 0, true}, sequence);
            journal_test::refreshChecksum(suffix, identity);
            if (tail == "partial") suffix.resize(7);
            if (tail == "corrupt") suffix.back() ^= std::byte{1};
            LinuxFile raw(journalDir, "journal.log");
            raw.writeAllAt(publishedEnd, suffix);
            raw.sync();
            auto recovered = StorageEngine::open(root);
            require(readFile(target) == before, "Published undo prefix was not restored");
            require(recovered.tableExists("victim"), "Unpublished record was replayed");
            require(!std::filesystem::exists(journalPath), "Recovered journal was not retired");
        }
    }

    void testInvalidNewestBoundaryDoesNotFallBack()
    {
        TestDirectory directory{"db-invalid-boundary"};
        const auto root = directory.path / "db";
        const auto journalPath = root / "journal/journal.log";
        const auto target = root / "tables/items.table";
        {
            auto database = Database::create(root, "test");
            database.executeSql("CREATE TABLE items (id INT);");
            database.executeSql("INSERT INTO items VALUES (1);");
        }
        {
            auto engine = StorageEngine::open(root);
            engine.beginStatement();
            auto table = engine.openTable("items");
            table.insertRows({.tableName = "items", .rows = {Row{{std::int32_t{2}}}}});
        }
        const auto changed = readFile(target);
        const auto original = readFile(journalPath);
        const journal_test::DatabaseIdentity db{root};
        LinuxDirectory journalDir{root / "journal"};
        JournalFile reader(journalDir, "journal.log", db.bytes, journal_test::StatementId, LinuxFile::OpenMode::ReadOnly);
        const auto identity = reader.readJournalHeader();
        for (const auto end : {std::uint64_t{journal_test::DataOffset - 1},
                               std::uint64_t{original.size() + 1},
                               std::uint64_t{journal_test::DataOffset + 1},
                               std::uint64_t{original.size()}})
        {
            auto bytes = original;
            const auto newest = journal_test::encodeBoundary(10, end, identity);
            const auto older = journal_test::encodeBoundary(9, journal_test::DataOffset, identity);
            std::copy(newest.begin(), newest.end(), bytes.begin() + journal_test::HeaderSize);
            std::copy(older.begin(), older.end(), bytes.begin() + journal_test::HeaderSize + journal_test::BoundarySize);
            if (end == original.size()) bytes.back() ^= std::byte{1};
            LinuxFile raw(journalDir, "journal.log");
            raw.writeAllAt(0, bytes);
            requireError([&] { (void)StorageEngine::open(root); },
                         end < journal_test::DataOffset || end > original.size() ? "endoffset" :
                         end == original.size() ? "checksum" : "Truncated");
            require(readFile(target) == changed, "Invalid newest boundary caused partial undo");
            require(readFile(journalPath) == bytes, "Invalid newest boundary discarded recovery evidence");
        }
    }

    void testLiveRollbackChecksStatementIdentity()
    {
        TestDirectory directory{"db-live-identity"};
        const auto root = directory.path / "db";
        auto engine = StorageEngine::create(root);
        engine.beginStatement();
        { auto table = engine.createTable("items", "test", {}, {}); }
        const auto journalPath = root / "journal/journal.log";
        const auto original = readFile(journalPath);
        auto changed = original;
        changed[32] ^= std::byte{1}; // Statement UUID; keep the header CRC valid.
        journal_test::setUnsigned(changed, 56, journal_test::crc(std::span<const std::byte>{changed}.first(56)));
        LinuxFile raw(LinuxDirectory{root / "journal"}, "journal.log");
        raw.writeAllAt(0, changed);
        const auto tableBefore = readFile(root / "tables/items.table");
        requireError([&] { engine.rollbackStatement(); }, "another statement");
        require(readFile(root / "tables/items.table") == tableBefore, "Identity mismatch modified a table");
        require(readFile(journalPath) == changed, "Identity mismatch discarded the journal");
        raw.writeAllAt(0, original);
        engine.rollbackStatement();
        require(!engine.tableExists("items"), "Rollback retry did not reset record sequence");
        require(!std::filesystem::exists(journalPath), "Rollback retry did not retire the journal");
    }
}

int main()
{
    try
    {
        testStorageIntegration();
        testMetadataValidation();
        testRestartValidationBeforeReplay();
        testUnpublishedTailsAreIgnored();
        testInvalidNewestBoundaryDoesNotFallBack();
        testLiveRollbackChecksStatementIdentity();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
