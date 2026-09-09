#pragma once

#include "db_query_result.h"

struct BoundSelect;
class Table;

class SelectExecutor
{
public:
    QueryResult execute(const BoundSelect &select, Table &table) const;
};
