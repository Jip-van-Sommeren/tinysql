#include "db_expression_evaluator.h"

#include <stdexcept>
#include <string>
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
        if (std::holds_alternative<int>(left) &&
            std::holds_alternative<int>(right))
        {
            int leftInt = std::get<int>(left);
            int rightInt = std::get<int>(right);

            switch (op)
            {
            case BinaryOperator::Gt:
                return leftInt > rightInt;
            case BinaryOperator::Ge:
                return leftInt >= rightInt;
            case BinaryOperator::Lt:
                return leftInt < rightInt;
            case BinaryOperator::Le:
                return leftInt <= rightInt;
            case BinaryOperator::Eq:
                return leftInt == rightInt;
            case BinaryOperator::Ne:
                return leftInt != rightInt;
            default:
                throw std::runtime_error("Unsupported comparison operator");
            }
        }

        if (std::holds_alternative<std::string>(left) &&
            std::holds_alternative<std::string>(right))
        {
            const std::string &leftString = std::get<std::string>(left);
            const std::string &rightString = std::get<std::string>(right);

            switch (op)
            {
            case BinaryOperator::Gt:
                return leftString > rightString;
            case BinaryOperator::Ge:
                return leftString >= rightString;
            case BinaryOperator::Lt:
                return leftString < rightString;
            case BinaryOperator::Le:
                return leftString <= rightString;
            case BinaryOperator::Eq:
                return leftString == rightString;
            case BinaryOperator::Ne:
                return leftString != rightString;
            default:
                throw std::runtime_error("Unsupported comparison operator");
            }
        }

        throw std::runtime_error("Cannot compare values of different types");
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
