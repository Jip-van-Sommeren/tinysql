#pragma once

#include "db_query_result.h"
#include "db_query_validator.h"

struct BoundSelect;
class Table;

class SelectExecutor
{
public:
    QueryResult execute(const BoundSelect &select, StorageEngine &storageEngine) const;
    std::pair<std::vector<Row>, size_t> executeTableRef(
        const BoundTableRef &tableRef,
        StorageEngine &storageEngine) const;
};

class QueryExecutor final
{
public:
    QueryExecutor(StorageEngine &storageEngine, const std::string &dbName)
        : storageEngine_(storageEngine), dbName_(dbName)
    {
    }

    QueryResult execute(const BoundQuery &query);

private:
    StorageEngine &storageEngine_;
    std::string dbName_;
    SelectExecutor selectExecutor_;

    QueryResult executeStatement(const BoundSelect &select);
    QueryResult executeStatement(const BoundInsert &insert);
    QueryResult executeStatement(const BoundDelete &del);
    QueryResult executeStatement(const BoundCreateTable &create);
    QueryResult executeStatement(const BoundUpdate &update);
    QueryResult executeStatement(const BoundCreateIndex &createIndex);
};