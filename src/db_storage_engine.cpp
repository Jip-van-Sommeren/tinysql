#include "db_storage_engine.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

StorageEngine::StorageEngine(
    const std::filesystem::path &path)
    : dbPath_(path),
      tablesDirectory_(dbPath_ / "tables"),
      journalDirectory_(dbPath_ / "journal")
{
}

StorageEngine StorageEngine::create(
    const std::filesystem::path &path)
{
    // Create the database root before creating its subdirectories.

    if (::mkdir(path.c_str(), 0700) == -1)
    {
        const int error = errno;

        throwError("mkdir", error, path);
    }

    if (::mkdir((path / "tables").c_str(), 0700) == -1)
    {
        const int error = errno;

        throwError("mkdir", error, path / "tables");
    }

    if (::mkdir((path / "journal").c_str(), 0700) == -1)
    {
        const int error = errno;

        throwError("mkdir", error, path / "journal");
    }

    LinuxDirectory root{
        path};

    return StorageEngine{
        path};
}

StorageEngine StorageEngine::open(
    const std::filesystem::path &path)
{
    // Opening either missing subdirectory throws; nothing is created.
    return StorageEngine{
        path};
}

bool StorageEngine::tableExists(const std::string &name) const
{
    return std::filesystem::exists(getTablePath(name));
}
HeaderPage StorageEngine::getTableHeader(const std::string &name) const
{
    //     HeaderPage getTableHeader(const std::string &tableName) const override
    //     {
    std::filesystem::path path = getTablePath(name);

    if (!std::filesystem::exists(path))
    {
        throw std::runtime_error("Table does not exist: " + name);
    }

    RawPage rawPage = readPageFromFile(path, 0);
    Page page = decodeHeaderPage(rawPage);

    return std::get<HeaderPage>(page.data);
}

Table StorageEngine::createTable(
    const std::string &tableName,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<Constraint> &constraints)
{
    std::filesystem::path tablePath = dbPath_ / "tables" / (tableName + ".table");

    if (std::filesystem::exists(tablePath))
    {
        throw std::runtime_error("Table already exists: " + tableName);
    }

    return Table::create(
        std::move(tablePath),
        tableName,
        magic,
        columns,
        constraints);
}

Table StorageEngine::openTable(const std::string &tableName)
{
    std::filesystem::path tablePath = dbPath_ / "tables" / (tableName + ".table");

    if (!std::filesystem::exists(tablePath))
    {
        throw std::runtime_error("Table does not exist: " + tableName);
    }

    return Table::open(std::move(tablePath));
}
