#include "db_storage_engine.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

StorageEngine::StorageEngine(std::filesystem::path dbPath)
    : dbPath(std::move(dbPath)),
      tablesPath(this->dbPath / "tables")
{
    std::filesystem::create_directories(tablesPath);
}

Table StorageEngine::createTable(
    const std::string &tableName,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<Constraint> &constraints)
{
    std::filesystem::path tablePath = getTablePath(tableName);

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

const std::filesystem::path &StorageEngine::getTablesPath() const
{
    return tablesPath;
}

std::filesystem::path StorageEngine::getTablePath(
    const std::string &tableName) const
{
    return tablesPath / (tableName + ".table");
}
