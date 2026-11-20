#pragma once

#include "db_query_error.h"
#include "db_query_result.h"
#include "db_query_validator.h"
#include "db_storage_engine.h"

#include <filesystem>
#include <expected>
#include <string>
#include <vector>

class Database
{
public:
    static Database create(const std::filesystem::path &path, const std::string &name);
    static Database open(const std::filesystem::path &path, const std::string &name);

    QueryResult executeSql(const std::string &sql);

    // Returns expected SQL rejections; storage/recovery/internal failures throw.
    // A rejected write has been rolled back before an error value is returned.
    [[nodiscard]] std::expected<QueryResult, QueryError> tryExecuteSql(const std::string &sql);

private:
    Database(StorageEngine engine, const std::string &name);

    std::string dbName;
    StorageEngine storageEngine;
};
