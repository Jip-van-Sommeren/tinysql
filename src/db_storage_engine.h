#pragma once

#include "db_table.h"

#include <filesystem>
#include <string>
#include <vector>

class StorageEngine
{
public:
    explicit StorageEngine(std::filesystem::path dbPath);

    Table createTable(
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns,
        const std::vector<Constraint> &constraints);
    Table openTable(const std::string &tableName);

    const std::filesystem::path &getTablesPath() const;

private:
    std::filesystem::path dbPath;
    std::filesystem::path tablesPath;

    std::filesystem::path getTablePath(const std::string &tableName) const;
};
