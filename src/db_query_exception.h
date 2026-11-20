#pragma once

#include "db_query_error.h"

#include <stdexcept>
#include <string>

// Internal propagation preserves stack unwinding and statement rollback. Only
// these explicitly classified rejections become QueryError values at the API.
class QueryException : public std::runtime_error
{
public:
    QueryException(QueryErrorCode code, const std::string &message)
        : std::runtime_error(message), code_(code) {}

    QueryErrorCode code() const noexcept { return code_; }

private:
    QueryErrorCode code_;
};

// Retain overflow_error compatibility for numeric callers without classifying
// unrelated storage overflows (page IDs, journal generations, etc.) as bad SQL.
class QueryNumericOverflow : public std::overflow_error
{
public:
    using std::overflow_error::overflow_error;
};
