#include "db_expression_evaluator.h"
#include "db_decimal.h"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

namespace
{
    template <typename T>
    constexpr bool isExactNumericValue =
        std::is_same_v<T, std::int32_t> ||
        std::is_same_v<T, std::int64_t> ||
        std::is_same_v<T, DecimalValue>;

    template <typename T>
    constexpr bool isNumericValue =
        isExactNumericValue<T> || std::is_same_v<T, std::float64_t>;

    template <typename T>
    DecimalValue asDecimal(const T &value)
    {
        if constexpr (std::is_same_v<T, DecimalValue>)
        {
            return value;
        }
        else
        {
            return decimalFromInt64(static_cast<std::int64_t>(value));
        }
    }

    bool applyComparison(
        BinaryOperator op,
        const auto &left,
        const auto &right)
    {
        switch (op)
        {
        case BinaryOperator::Gt:
            return left > right;
        case BinaryOperator::Ge:
            return left >= right;
        case BinaryOperator::Lt:
            return left < right;
        case BinaryOperator::Le:
            return left <= right;
        case BinaryOperator::Eq:
            return left == right;
        case BinaryOperator::Ne:
            return left != right;
        default:
            throw std::runtime_error("Unsupported comparison operator");
        }
    }

    Value evaluateValue(const BoundExpr &expr, const Row &row)
    {
        if (const auto *column = dynamic_cast<const BoundColumnExpr *>(&expr))
        {
            return row.values[column->columnIndex];
        }

        if (const auto *literal = dynamic_cast<const BoundLiteralExpr *>(&expr))
        {
            return literal->value;
        }

        throw std::runtime_error("Expression does not evaluate to a value");
    }

    bool compareValues(BinaryOperator op, const Value &left, const Value &right)
    {
        return std::visit(
            [op](const auto &lhs, const auto &rhs) -> bool
            {
                using L = std::decay_t<decltype(lhs)>;
                using R = std::decay_t<decltype(rhs)>;

                if constexpr (!std::is_same_v<L, R>)
                {
                    if constexpr (isExactNumericValue<L> &&
                                  isExactNumericValue<R>)
                    {
                        return applyComparison(
                            op,
                            asDecimal(lhs),
                            asDecimal(rhs));
                    }
                    else if constexpr (isNumericValue<L> && isNumericValue<R>)
                    {
                        const auto asFloat = [](const auto &value)
                        {
                            using T = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<T, DecimalValue>)
                            {
                                return decimalToFloat64(value);
                            }
                            else
                            {
                                return static_cast<std::float64_t>(value);
                            }
                        };
                        return applyComparison(
                            op,
                            asFloat(lhs),
                            asFloat(rhs));
                    }
                    else
                    {
                        throw std::runtime_error(
                            "Cannot compare values of different types");
                    }
                }
                else if constexpr (std::is_same_v<L, std::monostate>)
                {
                    throw std::runtime_error("Cannot directly compare NULL");
                }
                else
                {
                    return applyComparison(op, lhs, rhs);
                }
            },
            left,
            right);
    }

    bool evaluateBinaryPredicate(const BoundBinaryExpr &expr, const Row &row)
    {
        switch (expr.op)
        {
        case BinaryOperator::And:
            return evaluatePredicate(*expr.left, row) &&
                   evaluatePredicate(*expr.right, row);
        case BinaryOperator::Or:
            return evaluatePredicate(*expr.left, row) ||
                   evaluatePredicate(*expr.right, row);

        case BinaryOperator::Eq:
        case BinaryOperator::Ne:
        case BinaryOperator::Gt:
        case BinaryOperator::Ge:
        case BinaryOperator::Lt:
        case BinaryOperator::Le:
        {
            Value leftValue = evaluateValue(*expr.left, row);
            Value rightValue = evaluateValue(*expr.right, row);
            return compareValues(expr.op, leftValue, rightValue);
        }

        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
        case BinaryOperator::Multiply:
        case BinaryOperator::Divide:
            throw std::runtime_error(
                "Arithmetic operations should be folded during binding");
        }

        throw std::runtime_error("Unsupported binary predicate operator");
    }
}

bool evaluatePredicate(const BoundExpr &expr, const Row &row)
{
    if (const auto *binary = dynamic_cast<const BoundBinaryExpr *>(&expr))
    {
        return evaluateBinaryPredicate(*binary, row);
    }

    if (const auto *unary = dynamic_cast<const BoundUnaryExpr *>(&expr))
    {
        if (unary->op == UnaryOperator::Not)
        {
            return !evaluatePredicate(*unary->operand, row);
        }
    }

    if (const auto *isNull = dynamic_cast<const BoundIsNullExpr *>(&expr))
    {
        Value value = evaluateValue(*isNull->operand, row);
        bool result = std::holds_alternative<std::monostate>(value);
        return isNull->negated ? !result : result;
    }

    throw std::runtime_error("Expression does not evaluate to a predicate");
}
