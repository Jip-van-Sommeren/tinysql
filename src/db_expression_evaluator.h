#pragma once

#include "db_storage.h"
#include "db_types.h"

bool evaluatePredicate(const BoundExpr &expr, const Row &row);
