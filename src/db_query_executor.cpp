#include "db_query_executor.h"
#include "db_decimal.h"
#include "db_expression_evaluator.h"
#include "db_query_validator.h"
#include "db_table.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

    struct AggregateState
    {
        std::unique_ptr<BoundExpr> expression;
        Value value;
    };

    // Replace aggregate calls in a cloned projection with references to the
    // aggregate result row, so ordinary expression evaluation can finish it.
    void prepareAggregateProjection(
        std::unique_ptr<BoundExpr> &expr,
        std::vector<AggregateState> &aggregates)
    {
        switch (expr->kind())
        {
        case BoundExprKind::FunctionCall:
        {
            auto &function = static_cast<BoundFunctionCall &>(*expr);
            if (function.category == FunctionCategory::Aggregate)
            {
                if (function.id != FunctionId::Count &&
                    function.id != FunctionId::Sum)
                {
                    throw std::runtime_error("Aggregate function is not supported yet");
                }
                if (function.starArgument
                        ? function.id != FunctionId::Count || !function.arguments.empty()
                        : function.arguments.size() != 1)
                {
                    throw std::runtime_error("Invalid aggregate arguments");
                }
                for (const auto &argument : function.arguments)
                {
                    if (containsAggregate(*argument))
                    {
                        throw std::runtime_error("Nested aggregate functions are not allowed");
                    }
                }

                const DataType resultType = function.type();
                const auto index = static_cast<std::uint32_t>(aggregates.size());
                Value initial = function.id == FunctionId::Count
                    ? Value{std::int64_t{0}}
                    : Value{std::monostate{}};
                aggregates.push_back({std::move(expr), std::move(initial)});
                expr = std::make_unique<BoundColumnExpr>(index, resultType);
                return;
            }

            for (auto &argument : function.arguments)
            {
                prepareAggregateProjection(argument, aggregates);
            }
            return;
        }

        case BoundExprKind::Binary:
        {
            auto &binary = static_cast<BoundBinaryExpr &>(*expr);
            prepareAggregateProjection(binary.left, aggregates);
            prepareAggregateProjection(binary.right, aggregates);
            return;
        }

        case BoundExprKind::Unary:
            prepareAggregateProjection(
                static_cast<BoundUnaryExpr &>(*expr).operand, aggregates);
            return;

        case BoundExprKind::IsNull:
            prepareAggregateProjection(
                static_cast<BoundIsNullExpr &>(*expr).operand, aggregates);
            return;

        case BoundExprKind::ColumnReference:
            throw std::runtime_error(
                "Column reference outside an aggregate requires GROUP BY");

        case BoundExprKind::Literal:
            return;
        }

        throw std::runtime_error("Unknown bound expression kind");
    }

    std::int64_t checkedAggregateAdd(std::int64_t left, std::int64_t right)
    {
        if ((right > 0 &&
             left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 &&
             left < std::numeric_limits<std::int64_t>::min() - right))
        {
            throw std::overflow_error("Aggregate integer overflow");
        }
        return left + right;
    }

    Value addToSum(const Value &sum, const Value &value)
    {
        const bool firstValue = std::holds_alternative<std::monostate>(sum);
        return std::visit(
            [&sum, firstValue](const auto &inner) -> Value
            {
                using T = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<T, std::int32_t> ||
                              std::is_same_v<T, std::int64_t>)
                {
                    return checkedAggregateAdd(
                        firstValue ? 0 : std::get<std::int64_t>(sum),
                        static_cast<std::int64_t>(inner));
                }
                else if constexpr (std::is_same_v<T, std::float32_t> ||
                                   std::is_same_v<T, std::float64_t>)
                {
                    return (firstValue ? std::float64_t{0}
                                       : std::get<std::float64_t>(sum)) +
                           static_cast<std::float64_t>(inner);
                }
                else if constexpr (std::is_same_v<T, DecimalValue>)
                {
                    return firstValue ? inner
                                      : addDecimals(std::get<DecimalValue>(sum), inner);
                }
                else
                {
                    throw std::runtime_error("SUM requires a numeric argument");
                }
            },
            value);
    }

    void accumulateAggregate(AggregateState &aggregate, const Row &row)
    {
        const auto &function =
            static_cast<const BoundFunctionCall &>(*aggregate.expression);
        if (!function.starArgument)
        {
            Value value = evaluateValue(*function.arguments[0], row);
            if (std::holds_alternative<std::monostate>(value))
            {
                return;
            }
            if (function.id == FunctionId::Sum)
            {
                aggregate.value = addToSum(aggregate.value, value);
                return;
            }
        }

        aggregate.value = checkedAggregateAdd(
            std::get<std::int64_t>(aggregate.value), 1);
    }

    ResultRow evaluateAggregates(const BoundSelect &select, Table &table)
    {
        std::vector<AggregateState> aggregates;
        std::vector<std::unique_ptr<BoundExpr>> projections;
        projections.reserve(select.projections.size());
        for (const BoundSelectItem &projection : select.projections)
        {
            auto expr = projection.expr->clone();
            prepareAggregateProjection(expr, aggregates);
            projections.push_back(std::move(expr));
        }

        TableCursor cursor = table.scan();
        while (auto row = cursor.next())
        {
            if (select.where && !evaluatePredicate(*select.where, *row))
            {
                continue;
            }
            for (AggregateState &aggregate : aggregates)
            {
                accumulateAggregate(aggregate, *row);
            }
        }

        Row aggregateRow;
        aggregateRow.values.reserve(aggregates.size());
        for (AggregateState &aggregate : aggregates)
        {
            aggregateRow.values.push_back(std::move(aggregate.value));
        }

        ResultRow result;
        result.values.reserve(projections.size());
        for (const auto &projection : projections)
        {
            result.values.push_back(evaluateValue(*projection, aggregateRow));
        }
        return result;
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

    bool hasAggregate = false;
    for (const BoundSelectItem &projection : select.projections)
    {
        hasAggregate = hasAggregate || containsAggregate(*projection.expr);
        result.columns.push_back({
            projection.outputName,
            projection.expr->type()});
    }

    if (hasAggregate)
    {
        result.rows.push_back(evaluateAggregates(select, table));
        return result;
    }

    TableCursor cursor = table.scan();
    while (auto row = cursor.next())
    {
        if (select.where && !evaluatePredicate(*select.where, *row))
        {
            continue;
        }

        ResultRow resultRow;
        resultRow.values.reserve(select.projections.size());

        for (const BoundSelectItem &projection : select.projections)
        {
            resultRow.values.push_back(
                evaluateValue(*projection.expr, *row));
        }

        result.rows.push_back(std::move(resultRow));
    }

    return result;
}
