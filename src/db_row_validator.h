#pragma once

#include "db_storage.h"

#include <vector>

struct RowValidationResult
{
    bool valid;
    std::optional<size_t> columnIndex;
    std::string message;
};

RowValidationResult validateRowAgainstSchema(
    const std::vector<Column> &columns,
    const Row &row);
