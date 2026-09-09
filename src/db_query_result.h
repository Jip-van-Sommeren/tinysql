#pragma once

#include "db_types.h"

#include <cstdint>
#include <string>
#include <vector>

struct ResultColumn
{
    std::string name;
    DataType type;
};

struct ResultRow
{
    std::vector<Value> values;
};

struct QueryResult
{
    std::vector<ResultColumn> columns;
    std::vector<ResultRow> rows;
    std::uint64_t affectedRows = 0;
    bool returnsRows = false;
};
