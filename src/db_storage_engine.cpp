#include "db_storage_engine.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

StorageEngine::StorageEngine(const std::filesystem::path &path, LinuxFile::OpenMode mode)
    : table_dir_(path / "tables", mode),
      journal_dir_(path / "journal", mode),
      dbPath_(std::move(path))
{
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
    std::filesystem::path tablePath = getTablePath(tableName);

    if (!std::filesystem::exists(tablePath))
    {
        throw std::runtime_error("Table does not exist: " + tableName);
    }

    return Table::open(std::move(tablePath));
}

// const std::filesystem::path &StorageEngine::getTablesPath() const
// {
//     return tablesPath;
// }

std::filesystem::path StorageEngine::getTablePath(
    const std::string &tableName) const
{
    return dbPath / "tables" / (tableName + ".table");
}
