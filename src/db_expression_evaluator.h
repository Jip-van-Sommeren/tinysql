#pragma once

#include "db_storage.h"
#include "db_types.h"

Value evaluateValue(const BoundExpr &expr, const Row &row);
bool evaluatePredicate(const BoundExpr &expr, const Row &row);
