#include "db_expression_evaluator.h"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

namespace
{
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
                    throw std::runtime_error(
                        "Cannot compare values of different types");
                }
                else if constexpr (std::is_same_v<L, std::monostate>)
                {
                    throw std::runtime_error("Cannot directly compare NULL");
                }
                else
                {
                    switch (op)
                    {
                    case BinaryOperator::Gt:
                        return lhs > rhs;
                    case BinaryOperator::Ge:
                        return lhs >= rhs;
                    case BinaryOperator::Lt:
                        return lhs < rhs;
                    case BinaryOperator::Le:
                        return lhs <= rhs;
                    case BinaryOperator::Eq:
                        return lhs == rhs;
                    case BinaryOperator::Ne:
                        return lhs != rhs;
                    default:
                        throw std::runtime_error(
                            "Unsupported comparison operator");
                    }
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
