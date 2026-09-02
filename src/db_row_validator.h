#pragma once

#include "db_storage.h"

#include <vector>

RowValidationResult validateRowAgainstSchema(
    const std::vector<Column> &columns,
    const Row &row);
