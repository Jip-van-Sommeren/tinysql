#pragma once

#include <string>

enum class QueryErrorCode
{
    Syntax,
    Validation,
    Constraint,
    Numeric
};

// An expected rejection of the submitted SQL, suitable for reporting to a caller.
struct QueryError
{
    QueryErrorCode code;
    std::string message;
};
