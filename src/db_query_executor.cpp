#include "db_query_executor.h"
#include "db_expression_evaluator.h"
#include "db_query_validator.h"
#include "db_table.h"



QueryResult SelectExecutor::execute(
    const BoundSelect& select,
    const Table& table)
{
    QueryResult result;

    // Output schema comes from projections.
    for (const auto& projection : select.projections)
    {
        result.columns.push_back({
            projection.outputName,
            projection.expr->type()
        });
    }

    for (const Row& row : table.scan())
    {
        if (select.where &&
            !evaluatePredicate(*select.where, row))
        {
            continue;
        }

        ResultRow resultRow;

        for (const auto& projection : select.projections)
        {
            resultRow.values.push_back(
                evaluateValue(
                    *projection.expr,
                    row));
        }

        result.rows.push_back(
            std::move(resultRow));
    }

    return result;
}