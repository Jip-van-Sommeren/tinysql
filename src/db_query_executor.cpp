#include "db_query_executor.h"
#include "db_expression_evaluator.h"
#include "db_query_validator.h"
#include "db_table.h"

#include <stdexcept>
#include <utility>

namespace
{
    bool containsAggregate(const BoundExpr &expr)
    {
        switch (expr.kind())
        {
        case BoundExprKind::FunctionCall:
        {
            const auto &function =
                static_cast<const BoundFunctionCall &>(expr);
            if (function.category == FunctionCategory::Aggregate)
            {
                return true;
            }
            for (const auto &argument : function.arguments)
            {
                if (containsAggregate(*argument))
                {
                    return true;
                }
            }
            return false;
        }

        case BoundExprKind::Binary:
        {
            const auto &binary = static_cast<const BoundBinaryExpr &>(expr);
            return containsAggregate(*binary.left) ||
                   containsAggregate(*binary.right);
        }

        case BoundExprKind::Unary:
            return containsAggregate(
                *static_cast<const BoundUnaryExpr &>(expr).operand);

        case BoundExprKind::IsNull:
            return containsAggregate(
                *static_cast<const BoundIsNullExpr &>(expr).operand);

        case BoundExprKind::Literal:
        case BoundExprKind::ColumnReference:
            return false;
        }

        throw std::runtime_error("Unknown bound expression kind");
    }
}

QueryResult SelectExecutor::execute(
    const BoundSelect &select,
    Table &table) const
{
    if (!select.groupBy.empty() || select.having)
    {
        throw std::runtime_error(
            "GROUP BY and HAVING are not supported yet");
    }

    if (select.where && containsAggregate(*select.where))
    {
        throw std::runtime_error(
            "Aggregate functions are not supported in WHERE");
    }

    QueryResult result{
        .columns = {},
        .rows = {},
        .affectedRows = 0,
        .returnsRows = true};

    result.columns.reserve(select.projections.size());
    for (const BoundSelectItem &projection : select.projections)
    {
        if (containsAggregate(*projection.expr))
        {
            throw std::runtime_error(
                "Aggregate functions are not supported yet");
        }
        result.columns.push_back({
            projection.outputName,
            projection.expr->type()
        });
    }

    for (const Row &row : table.scan())
    {
        if (select.where && !evaluatePredicate(*select.where, row))
        {
            continue;
        }

        ResultRow resultRow;
        resultRow.values.reserve(select.projections.size());

        for (const BoundSelectItem &projection : select.projections)
        {
            resultRow.values.push_back(
                evaluateValue(*projection.expr, row));
        }

        result.rows.push_back(std::move(resultRow));
    }

    return result;
}
