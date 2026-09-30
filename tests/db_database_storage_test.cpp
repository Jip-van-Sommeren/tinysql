#include "db_database.h"
#include "db_linux_file.h"
#include "db_storage_engine.h"
#include "db_test_directory.h"

#include <iostream>
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
        LinuxFile file{path, LinuxFile::OpenMode::ReadOnly};
        std::vector<std::byte> bytes(static_cast<std::size_t>(file.size()));
        file.readExactAt(0, bytes);
        return bytes;
    }

    void testStorageIntegration()
    {
        TestDirectory directory{"db-storage-integration-test"};
        const auto root = directory.path / "db";
        Database database = Database::create(root, "test");

        const auto unrelated = directory.path / "unrelated";
        std::filesystem::create_directories(unrelated / "tables");
        {
            LinuxFile decoy{unrelated / "tables/items.table", LinuxFile::OpenMode::CreateNew};
            LinuxFile missing{unrelated / "tables/missing.table", LinuxFile::OpenMode::CreateNew};
        }
        WorkingDirectory workingDirectory{unrelated};

        // An unrelated file with the same relative name must not block creation.
        database.executeSql("CREATE TABLE items (id INT NOT NULL);");
        const auto tablePath = root / "tables/items.table";
        const auto journalPath = root / "journal/journal.log";
        require(std::filesystem::exists(tablePath), "Table was not created in the database root");

        {
            StorageEngine engine = StorageEngine::open(root);
            require(engine.tableExists("items"), "Catalog did not resolve the database root");
            requireError([&] { engine.createTable("items", "test", {}, {}); }, "Table already exists");
            requireError([&] { engine.openTable("missing"); }, "Table does not exist");
            require(!std::filesystem::exists(root / "tables/missing.table"),
                    "Opening a missing table created it");
        }

        database.insertRows("items", {Row{{std::int32_t{1}}}});
        require(!std::filesystem::exists(journalPath), "Successful direct INSERT left a journal");
        const auto before = readFile(tablePath);

        // The first row is valid and modifies cached pages before the second
        // row fails validation. The direct API must take the rollback path.
        requireError([&]
        {
            database.insertRows("items", {
                Row{{std::int32_t{2}}}, Row{{std::string{"not an integer"}}}});
        }, "expects type");
        require(readFile(tablePath) == before, "Failed direct INSERT changed the table bytes");
        require(!std::filesystem::exists(journalPath), "Rollback left an active journal");
        require(database.selectAllRows("items").size() == 1, "Failed INSERT retained a row");

        requireError([&] { database.insertRows("missing", {Row{{std::int32_t{3}}}}); },
                     "Table does not exist");
        require(!std::filesystem::exists(journalPath), "Failed table lookup left an active journal");

        database.insertRows("items", {});
        require(readFile(tablePath) == before && !std::filesystem::exists(journalPath),
                "Empty direct INSERT changed data or left a journal");
        database.insertRows("items", {Row{{std::int32_t{2}}}});

        Database reopened = Database::open(root, "test");
        const auto rows = reopened.selectAllRows("items");
        require(rows.size() == 2 && std::get<std::int32_t>(rows[0].values.at(0)) == 1 &&
                    std::get<std::int32_t>(rows[1].values.at(0)) == 2,
                "Direct INSERT did not recover for the next statement or survive reopening");
        require(std::filesystem::file_size(unrelated / "tables/items.table") == 0,
                "Database modified the working-directory decoy file");
    }
}

int main()
{
    try
    {
        testStorageIntegration();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
