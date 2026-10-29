#pragma once

#include "db_query_result.h"
#include "db_query_validator.h"
#include "db_storage_engine.h"

#include <filesystem>
#include <string>
#include <vector>

class Database
{
public:
    static Database create(const std::filesystem::path &path, const std::string &name);
    static Database open(const std::filesystem::path &path, const std::string &name);

    QueryResult executeSql(const std::string &sql);

private:
    Database(StorageEngine engine, const std::string &name);

    std::string dbName;
    StorageEngine storageEngine;
};
