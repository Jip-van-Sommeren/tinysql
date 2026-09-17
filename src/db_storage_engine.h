#pragma once

#include "db_table.h"

#include <filesystem>
#include <string>
#include <vector>

class StorageEngine
{
public:
    explicit StorageEngine(const std::filesystem::path &path, LinuxFile::OpenMode mode);

    Table createTable(
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns,
        const std::vector<Constraint> &constraints);
    Table openTable(const std::string &tableName);

    // const std::filesystem::path &getTablesPath() const;

private:
    LinuxFile table_dir_;
    LinuxFile journal_dir_;
    std::filesystem::path dbPath_;


    std::filesystem::path getTablePath(const std::string &tableName) const;
};
