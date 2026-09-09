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
    Database(std::filesystem::path dbPath, std::string name);

    void createTable(
        const std::string &tableName,
        const std::vector<Column> &columns,
        const std::vector<Constraint> &constraints);
    void insertRows(
        const std::string &tableName,
        const std::vector<Row> &rows);
    std::vector<Row> selectAllRows(const std::string &tableName);

    QueryResult execute(const BoundQuery &query);
    QueryResult executeSql(const std::string &sql);

private:
    std::string dbName;
    StorageEngine storageEngine;

    QueryResult executeInsert(const BoundInsert &insert);
    QueryResult executeSelect(const BoundSelect &select);
    QueryResult executeDelete(const BoundDelete &del);
    QueryResult executeCreateTable(const BoundCreateTable &createTable);
};
