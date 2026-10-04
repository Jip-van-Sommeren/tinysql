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
    const std::filesystem::path &path, LinuxDirectory lockedRoot)
    : dbPath_(path),
      dbRootDirectory_(std::move(lockedRoot)),
      tablesDirectory_(dbPath_ / "tables"),
      journalDirectory_(dbPath_ / "journal"),
      statementRecovery_(path)
{
}

StorageEngine StorageEngine::create(
    const std::filesystem::path &path)
{
    // Create the database root before creating its subdirectories.
    LinuxDirectory root{
        path};
    root.lockExclusive();
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

    root.sync();
    LinuxDirectory rootParent{
        std::filesystem::canonical(path).parent_path()};

    rootParent.sync();

    return StorageEngine{path, std::move(root)};
}

StorageEngine StorageEngine::open(
    const std::filesystem::path &path)
{
    LinuxDirectory root{path};
    root.lockExclusive();

    StorageEngine engine{path, std::move(root)};

    if (std::filesystem::exists(path / "journal" / "journal.log"))
    {
        engine.statementRecovery_.initializeForRecover();
        engine.statementRecovery_.rollback();
    }

    return engine;
}

bool StorageEngine::tableExists(const std::string &name) const
{
    return std::filesystem::exists(getTablePath(name));
}

HeaderPage StorageEngine::getTableHeader(const std::string &name) const
{
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
    const std::filesystem::path relativeTablePath =
        std::filesystem::path{"tables"} / (tableName + ".table");

    if (std::filesystem::exists(getTablePath(tableName)))
    {
        throw std::runtime_error("Table already exists: " + tableName);
    }

    return Table::create(
        dbPath_,
        relativeTablePath,
        tableName,
        magic,
        columns,
        constraints,
        statementRecovery_);
}

Table StorageEngine::openTable(const std::string &tableName)
{
    const std::filesystem::path relativeTablePath =
        std::filesystem::path{"tables"} / (tableName + ".table");

    if (!std::filesystem::exists(getTablePath(tableName)))
    {
        throw std::runtime_error("Table does not exist: " + tableName);
    }

    return Table::open(dbPath_, relativeTablePath, statementRecovery_);
}
