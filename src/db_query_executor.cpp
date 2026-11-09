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
#include <map>

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
    bool sameBoundExpr(const BoundExpr &a, const BoundExpr &b)
    {
        if (a.kind() != b.kind())
        {
            return false;
        }
        if (a.type() != b.type())
        {
            return false;
        }

        switch (a.kind())
        {
        case BoundExprKind::ColumnReference:
        {
            const auto &colA = static_cast<const BoundColumnExpr &>(a);
            const auto &colB = static_cast<const BoundColumnExpr &>(b);
            return colA.columnIndex == colB.columnIndex;
        }

        case BoundExprKind::Literal:
        {
            const auto &litA = static_cast<const BoundLiteralExpr &>(a);
            const auto &litB = static_cast<const BoundLiteralExpr &>(b);
            return litA.value == litB.value;
        }

        case BoundExprKind::Binary:
        {
            const auto &binA = static_cast<const BoundBinaryExpr &>(a);
            const auto &binB = static_cast<const BoundBinaryExpr &>(b);
            return binA.op == binB.op &&
                   sameBoundExpr(*binA.left, *binB.left) &&
                   sameBoundExpr(*binA.right, *binB.right);
        }

        case BoundExprKind::Unary:
        {
            const auto &unaryA = static_cast<const BoundUnaryExpr &>(a);
            const auto &unaryB = static_cast<const BoundUnaryExpr &>(b);
            return unaryA.op == unaryB.op &&
                   sameBoundExpr(*unaryA.operand, *unaryB.operand);
        }

        case BoundExprKind::FunctionCall:
        {
            const auto &funcA = static_cast<const BoundFunctionCall &>(a);
            const auto &funcB = static_cast<const BoundFunctionCall &>(b);
            if (funcA.id != funcB.id ||
                funcA.category != funcB.category ||
                funcA.arguments.size() != funcB.arguments.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < funcA.arguments.size(); ++i)
            {
                if (!sameBoundExpr(*funcA.arguments[i], *funcB.arguments[i]))
                {
                    return false;
                }
            }
            return true;
        }
        case BoundExprKind::IsNull:
        {
            const auto &isNullA = static_cast<const BoundIsNullExpr &>(a);
            const auto &isNullB = static_cast<const BoundIsNullExpr &>(b);
            return isNullA.negated == isNullB.negated &&
                   sameBoundExpr(*isNullA.operand, *isNullB.operand);
        }

        default:
            return false;
        }
    }

    // Replace aggregate calls in a cloned projection with references to the
    // aggregate result row, so ordinary expression evaluation can finish it.
    void prepareAggregateProjection(
        std::unique_ptr<BoundExpr> &expr,
        std::vector<AggregateState> &aggregates,
        const std::vector<std::unique_ptr<BoundExpr>> &groupKeys)
    {
        for (std::size_t i = 0; i < groupKeys.size(); ++i)
        {
            if (sameBoundExpr(*expr, *groupKeys[i]))
            {
                const auto type = expr->type();

                expr = std::make_unique<BoundColumnExpr>(
                    static_cast<std::uint32_t>(i),
                    type);

                return;
            }
        }
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
                const auto index = static_cast<std::uint32_t>(groupKeys.size() + aggregates.size());
                Value initial = function.id == FunctionId::Count
                                    ? Value{std::int64_t{0}}
                                    : Value{std::monostate{}};
                aggregates.push_back({std::move(expr), std::move(initial)});
                expr = std::make_unique<BoundColumnExpr>(index, resultType);
                return;
            }

            for (auto &argument : function.arguments)
            {
                prepareAggregateProjection(argument, aggregates, groupKeys);
            }
            return;
        }

        case BoundExprKind::Binary:
        {
            auto &binary = static_cast<BoundBinaryExpr &>(*expr);
            prepareAggregateProjection(binary.left, aggregates, groupKeys);
            prepareAggregateProjection(binary.right, aggregates, groupKeys);
            return;
        }

        case BoundExprKind::Unary:
            prepareAggregateProjection(
                static_cast<BoundUnaryExpr &>(*expr).operand, aggregates, groupKeys);
            return;

        case BoundExprKind::IsNull:
            prepareAggregateProjection(
                static_cast<BoundIsNullExpr &>(*expr).operand, aggregates, groupKeys);
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

    std::vector<ResultRow> evaluateAggregates(const BoundSelect &select, const std::vector<Row> &rows)
    {
        std::vector<AggregateState> aggregates;
        std::vector<std::unique_ptr<BoundExpr>> projections;
        projections.reserve(select.projections.size());
        for (const BoundSelectItem &projection : select.projections)
        {
            auto expr = projection.expr->clone();
            prepareAggregateProjection(expr, aggregates, select.groupBy);
            projections.push_back(std::move(expr));
        }

        std::unique_ptr<BoundExpr> preparedHaving;

        if (select.having)
        {
            preparedHaving = select.having->clone();
            prepareAggregateProjection(preparedHaving, aggregates, select.groupBy);
        }
        std::map<std::vector<Value>, std::vector<AggregateState>> groupAggregates{};

        for (const auto &row : rows)
        {
            if (select.where && !evaluatePredicate(*select.where, row))
            {
                continue;
            }
            if (select.groupBy.empty())
            {
                for (AggregateState &aggregate : aggregates)
                {
                    accumulateAggregate(aggregate, row);
                }
                continue;
            }
            std::vector<Value> groupKey{};
            groupKey.reserve(select.groupBy.size());

            for (const auto &groupByExpr : select.groupBy)
            {

                auto keyValue = evaluateValue(*groupByExpr, row);
                groupKey.push_back(keyValue);
            }
            if (!groupAggregates.contains(groupKey))
            {
                auto &states = groupAggregates[groupKey];
                states.reserve(aggregates.size());

                for (const auto &aggregate : aggregates)
                {
                    states.push_back({aggregate.expression->clone(),
                                      aggregate.value});
                }
            }
            for (AggregateState &aggregate : groupAggregates[groupKey])
            {
                accumulateAggregate(aggregate, row);
            }
        }

        if (select.groupBy.empty())
        {
            Row aggregateRow;

            aggregateRow.values.reserve(aggregates.size());
            for (AggregateState &aggregate : aggregates)
            {
                aggregateRow.values.push_back(std::move(aggregate.value));
            }
            // aggregateRows.push_back(std::move(aggregateRow));
            if (preparedHaving &&
                !evaluatePredicate(*preparedHaving, aggregateRow))
            {

                return {};
            }

            ResultRow result;
            result.values.reserve(projections.size());
            for (const auto &projection : projections)
            {
                result.values.push_back(evaluateValue(*projection, aggregateRow));
            }
            return {result};
        }

        std::vector<Row> aggregateRows;
        aggregateRows.reserve(groupAggregates.size());
        for (auto &[groupKey, groupAggregates] : groupAggregates)
        {
            Row groupRow;
            groupRow.values = std::move(groupKey);
            groupRow.values.reserve(groupRow.values.size() + groupAggregates.size());
            for (AggregateState &aggregate : groupAggregates)
            {
                groupRow.values.push_back(std::move(aggregate.value));
            }
            aggregateRows.push_back(std::move(groupRow));
        }

        std::vector<ResultRow> result{};
        result.reserve(groupAggregates.size());
        for (const auto &aggregateRow : aggregateRows)
        {
            if (preparedHaving &&
                !evaluatePredicate(*preparedHaving, aggregateRow))
            {
                continue;
            }
            ResultRow resultRow;
            resultRow.values.reserve(projections.size());
            for (const auto &projection : projections)
            {
                resultRow.values.push_back(evaluateValue(*projection, aggregateRow));
            }
            result.push_back(std::move(resultRow));
        }
        return result;
    }
}

std::pair<std::vector<Row>, std::size_t> SelectExecutor::executeTableRef(
    const BoundTableRef &tableRef,
    StorageEngine &storageEngine) const
{
    return std::visit(
        [&](const auto &join) -> std::pair<std::vector<Row>, std::size_t>
        {
            using T = std::decay_t<decltype(join)>;
            if constexpr (std::is_same_v<T, BoundNamedTableRef>)
            {
                auto table = storageEngine.openTable(join.tableName);
                const auto columnCount =
                    storageEngine.getTableHeader(join.tableName).columns.size();
                auto cursor = table.scan();
                std::vector<Row> result;

                while (auto row = cursor.next())
                {
                    result.push_back(*row);
                }
                return {std::move(result), columnCount};
            }
            else if constexpr (std::is_same_v<T, BoundJoinTableRef>)
            {

                auto [leftRows, leftColumnCount] =
                    executeTableRef(*join.left, storageEngine);

                auto [rightRows, rightColumnCount] =
                    executeTableRef(*join.right, storageEngine);

                std::vector<Row>
                    result;
                std::vector<bool> rightMatched(rightRows.size(), false);

                for (const Row &left : leftRows)
                {
                    auto matched{false};
                    for (std::size_t i = 0; i < rightRows.size(); ++i)
                    {
                        const Row &right = rightRows.at(i);
                        Row combined = left;
                        combined.values.insert(
                            combined.values.end(),
                            right.values.begin(),
                            right.values.end());

                        if (join.type == JoinType::Cross ||
                            evaluatePredicate(*join.condition, combined))
                        {
                            matched = true;
                            rightMatched.at(i) = true;
                            result.push_back(std::move(combined));
                        }
                    }
                    if (!matched &&
                        (join.type == JoinType::LeftOuter ||
                         join.type == JoinType::FullOuter))
                    {
                        Row combined = left;
                        combined.values.resize(
                            left.values.size() + rightColumnCount,
                            Value{std::monostate{}});

                        result.push_back(std::move(combined));
                    }
                }
                // A right row is unmatched only after every left row has
                // been checked. This also handles an empty left input.
                if (join.type == JoinType::RightOuter ||
                    join.type == JoinType::FullOuter)
                {
                    for (std::size_t i = 0; i < rightRows.size(); ++i)
                    {
                        if (rightMatched.at(i))
                            continue;

                        Row combined{};
                        combined.values.resize(leftColumnCount, Value{std::monostate{}});

                        const Row &right = rightRows[i];
                        combined.values.insert(
                            combined.values.end(),
                            right.values.begin(),
                            right.values.end());

                        result.push_back(std::move(combined));
                    }
                }
                return {std::move(result), leftColumnCount + rightColumnCount};
            }
        },
        tableRef.value

    );
}

QueryResult SelectExecutor::execute(
    const BoundSelect &select,
    StorageEngine &storageEngine) const
{

    if (select.where && containsAggregate(*select.where))
    {
        throw std::runtime_error(
            "Aggregate functions are not supported in WHERE");
    }
    const auto [rows, columnCount] = executeTableRef(*select.from, storageEngine);

    QueryResult result{
        .columns = {},
        .rows = {},
        .affectedRows = 0,
        .returnsRows = true};

    result.columns.reserve(select.projections.size());

    bool hasAggregate{false};
    for (const BoundSelectItem &projection : select.projections)
    {
        hasAggregate = hasAggregate || containsAggregate(*projection.expr);

        result.columns.push_back({projection.outputName,
                                  projection.expr->type()});
    }

    if (hasAggregate || !select.groupBy.empty() || select.having)
    {
        result.rows = evaluateAggregates(select, rows);
        return result;
    }

    for (const auto &row : rows)
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

QueryResult QueryExecutor::execute(const BoundQuery &query)
{
    storageEngine_.requireReadable();

    return std::visit(
        [this](const auto &statement) -> QueryResult
        {
            return executeStatement(statement);
        },
        query);
}

QueryResult QueryExecutor::executeStatement(
    const BoundSelect &select)
{
    if (!select.from)
        throw std::runtime_error("SELECT requires a FROM clause");

    return selectExecutor_.execute(select, storageEngine_);
}

QueryResult QueryExecutor::executeStatement(
    const BoundInsert &insert)
{
    storageEngine_.beginStatement();

    QueryResult result;
    try
    {
        result = storageEngine_.insertRows(insert);
    }
    catch (...)
    {
        storageEngine_.rollbackStatement();
        throw;
    }

    storageEngine_.commitStatement();
    return result;
}

QueryResult QueryExecutor::executeStatement(const BoundDelete &del)
{
    storageEngine_.beginStatement();
    QueryResult queryResult{};

    try
    {
        queryResult = storageEngine_.deleteRows(del);
    }
    catch (...)
    {
        storageEngine_.rollbackStatement();
        throw;
    }

    storageEngine_.commitStatement();

    return queryResult;
}

QueryResult QueryExecutor::executeStatement(
    const BoundCreateTable &createTable)
{
    storageEngine_.beginStatement();

    try
    {
        storageEngine_.createTable(
            createTable.tableName,
            dbName_,
            createTable.columns,
            createTable.constraints);
    }
    catch (...)
    {
        storageEngine_.rollbackStatement();
        throw;
    }

    storageEngine_.commitStatement();

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = 0,
        .returnsRows = false};
}

QueryResult QueryExecutor::executeStatement(
    const BoundCreateIndex &createIndex)
{
    storageEngine_.beginStatement();

    try
    {
        storageEngine_.createIndex(
            createIndex);
    }
    catch (...)
    {
        storageEngine_.rollbackStatement();
        throw;
    }

    storageEngine_.commitStatement();

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = 0,
        .returnsRows = false};
}

QueryResult QueryExecutor::executeStatement(const BoundUpdate &update)
{
    storageEngine_.beginStatement();
    QueryResult queryResult{};

    try
    {
        queryResult = storageEngine_.updateRows(update);
    }
    catch (...)
    {
        storageEngine_.rollbackStatement();
        throw;
    }

    storageEngine_.commitStatement();

    return queryResult;
}
