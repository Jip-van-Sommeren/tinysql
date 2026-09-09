#include "db_expression_evaluator.h"

#include "db_decimal.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{
    template <typename T>
    constexpr bool isExactNumericValue =
        std::is_same_v<T, std::int32_t> ||
        std::is_same_v<T, std::int64_t> ||
        std::is_same_v<T, DecimalValue>;

    template <typename T>
    constexpr bool isNumericValue =
        isExactNumericValue<T> ||
        std::is_same_v<T, std::float32_t> ||
        std::is_same_v<T, std::float64_t>;

    DecimalValue asDecimal(const Value &value)
    {
        if (const auto *decimal = std::get_if<DecimalValue>(&value))
        {
            return *decimal;
        }
        if (const auto *integer = std::get_if<std::int32_t>(&value))
        {
            return decimalFromInt64(*integer);
        }
        if (const auto *integer = std::get_if<std::int64_t>(&value))
        {
            return decimalFromInt64(*integer);
        }
        throw std::runtime_error("Expected an exact numeric value");
    }

    std::int64_t asInt64(const Value &value)
    {
        if (const auto *integer = std::get_if<std::int32_t>(&value))
        {
            return *integer;
        }
        if (const auto *integer = std::get_if<std::int64_t>(&value))
        {
            return *integer;
        }
        throw std::runtime_error("Expected an integer value");
    }

    std::float64_t asFloat64(const Value &value)
    {
        return std::visit(
            [](const auto &inner) -> std::float64_t
            {
                using T = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<T, DecimalValue>)
                {
                    return decimalToFloat64(inner);
                }
                else if constexpr (
                    std::is_same_v<T, std::int32_t> ||
                    std::is_same_v<T, std::int64_t> ||
                    std::is_same_v<T, std::float32_t> ||
                    std::is_same_v<T, std::float64_t>)
                {
                    return static_cast<std::float64_t>(inner);
                }
                else
                {
                    throw std::runtime_error("Expected a numeric value");
                }
            },
            value);
    }

    std::int64_t checkedAdd(std::int64_t left, std::int64_t right)
    {
        if ((right > 0 &&
             left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 &&
             left < std::numeric_limits<std::int64_t>::min() - right))
        {
            throw std::overflow_error("Integer expression overflow");
        }
        return left + right;
    }

    std::int64_t checkedSubtract(std::int64_t left, std::int64_t right)
    {
        if ((right > 0 &&
             left < std::numeric_limits<std::int64_t>::min() + right) ||
            (right < 0 &&
             left > std::numeric_limits<std::int64_t>::max() + right))
        {
            throw std::overflow_error("Integer expression overflow");
        }
        return left - right;
    }

    std::int64_t checkedMultiply(std::int64_t left, std::int64_t right)
    {
        if (left == 0 || right == 0)
        {
            return 0;
        }
        if ((left == -1 &&
             right == std::numeric_limits<std::int64_t>::min()) ||
            (right == -1 &&
             left == std::numeric_limits<std::int64_t>::min()))
        {
            throw std::overflow_error("Integer expression overflow");
        }
        if (left > 0)
        {
            if ((right > 0 &&
                 left > std::numeric_limits<std::int64_t>::max() / right) ||
                (right < 0 &&
                 right < std::numeric_limits<std::int64_t>::min() / left))
            {
                throw std::overflow_error("Integer expression overflow");
            }
        }
        else if ((right > 0 &&
                  left < std::numeric_limits<std::int64_t>::min() / right) ||
                 (right < 0 &&
                  left < std::numeric_limits<std::int64_t>::max() / right))
        {
            throw std::overflow_error("Integer expression overflow");
        }
        return left * right;
    }

    std::int64_t evaluateIntegerArithmetic(
        BinaryOperator op,
        std::int64_t left,
        std::int64_t right)
    {
        switch (op)
        {
        case BinaryOperator::Add:
            return checkedAdd(left, right);
        case BinaryOperator::Subtract:
            return checkedSubtract(left, right);
        case BinaryOperator::Multiply:
            return checkedMultiply(left, right);
        case BinaryOperator::Divide:
            if (right == 0)
            {
                throw std::runtime_error("Division by zero");
            }
            if (left == std::numeric_limits<std::int64_t>::min() &&
                right == -1)
            {
                throw std::overflow_error("Integer expression overflow");
            }
            return left / right;

        default:
            throw std::runtime_error("Unsupported arithmetic operator");
        }
    }

    Value evaluateArithmetic(
        BinaryOperator op,
        const Value &left,
        const Value &right,
        DataType resultType)
    {
        if (std::holds_alternative<std::monostate>(left) ||
            std::holds_alternative<std::monostate>(right))
        {
            return std::monostate{};
        }

        switch (resultType)
        {
        case DataType::Int:
        {
            const std::int64_t result = evaluateIntegerArithmetic(
                op,
                asInt64(left),
                asInt64(right));
            if (result < std::numeric_limits<std::int32_t>::min() ||
                result > std::numeric_limits<std::int32_t>::max())
            {
                throw std::overflow_error("Integer expression overflow");
            }
            return static_cast<std::int32_t>(result);
        }

        case DataType::BigInt:
            return evaluateIntegerArithmetic(
                op,
                asInt64(left),
                asInt64(right));

        case DataType::Float:
        case DataType::Double:
        {
            const std::float64_t lhs = asFloat64(left);
            const std::float64_t rhs = asFloat64(right);
            std::float64_t result;
            switch (op)
            {
            case BinaryOperator::Add:
                result = lhs + rhs;
                break;
            case BinaryOperator::Subtract:
                result = lhs - rhs;
                break;
            case BinaryOperator::Multiply:
                result = lhs * rhs;
                break;
            case BinaryOperator::Divide:
                if (rhs == 0)
                {
                    throw std::runtime_error("Division by zero");
                }
                result = lhs / rhs;
                break;
            default:
                throw std::runtime_error("Unsupported arithmetic operator");
            }

            if (resultType == DataType::Float)
            {
                return static_cast<std::float32_t>(result);
            }
            return result;
        }

        case DataType::Decimal:
        {
            const DecimalValue lhs = asDecimal(left);
            const DecimalValue rhs = asDecimal(right);
            switch (op)
            {
            case BinaryOperator::Add:
                return addDecimals(lhs, rhs);
            case BinaryOperator::Subtract:
                return subtractDecimals(lhs, rhs);
            case BinaryOperator::Multiply:
                return multiplyDecimals(lhs, rhs);
            case BinaryOperator::Divide:
                if (rhs.coefficient == 0)
                {
                    throw std::runtime_error("Division by zero");
                }
                return divideDecimals(lhs, rhs);
            default:
                throw std::runtime_error("Unsupported arithmetic operator");
            }
        }

        default:
            throw std::runtime_error("Arithmetic requires numeric operands");
        }
    }

    template <typename T>
    DecimalValue exactAsDecimal(const T &value)
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

    bool compareValues(
        BinaryOperator op,
        const Value &left,
        const Value &right)
    {
        return std::visit(
            [op](const auto &lhs, const auto &rhs) -> bool
            {
                using L = std::decay_t<decltype(lhs)>;
                using R = std::decay_t<decltype(rhs)>;

                if constexpr (std::is_same_v<L, std::monostate> ||
                              std::is_same_v<R, std::monostate>)
                {
                    throw std::runtime_error("Cannot directly compare NULL");
                }
                else if constexpr (!std::is_same_v<L, R>)
                {
                    if constexpr (isExactNumericValue<L> &&
                                  isExactNumericValue<R>)
                    {
                        return applyComparison(
                            op,
                            exactAsDecimal(lhs),
                            exactAsDecimal(rhs));
                    }
                    else if constexpr (isNumericValue<L> && isNumericValue<R>)
                    {
                        const auto toFloat = [](const auto &value)
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
                        return applyComparison(op, toFloat(lhs), toFloat(rhs));
                    }
                    else
                    {
                        throw std::runtime_error(
                            "Cannot compare values of different types");
                    }
                }
                else
                {
                    return applyComparison(op, lhs, rhs);
                }
            },
            left,
            right);
    }

    bool asBoolean(const Value &value, const char *context)
    {
        if (const auto *boolean = std::get_if<bool>(&value))
        {
            return *boolean;
        }
        throw std::runtime_error(
            std::string{context} + " expression did not produce a boolean");
    }

    Value evaluateAbs(const Value &value)
    {
        if (std::holds_alternative<std::monostate>(value))
        {
            return std::monostate{};
        }
        if (const auto *integer = std::get_if<std::int32_t>(&value))
        {
            if (*integer == std::numeric_limits<std::int32_t>::min())
            {
                throw std::overflow_error("ABS result is out of range");
            }
            return static_cast<std::int32_t>(std::abs(*integer));
        }
        if (const auto *integer = std::get_if<std::int64_t>(&value))
        {
            if (*integer == std::numeric_limits<std::int64_t>::min())
            {
                throw std::overflow_error("ABS result is out of range");
            }
            return static_cast<std::int64_t>(std::abs(*integer));
        }
        if (const auto *floating = std::get_if<std::float32_t>(&value))
        {
            return static_cast<std::float32_t>(std::fabs(*floating));
        }
        if (const auto *floating = std::get_if<std::float64_t>(&value))
        {
            return static_cast<std::float64_t>(std::fabs(*floating));
        }
        if (const auto *decimal = std::get_if<DecimalValue>(&value))
        {
            return decimalAbs(*decimal);
        }
        throw std::runtime_error("ABS requires a numeric argument");
    }

    Value evaluateScalarFunction(
        const BoundFunctionCall &function,
        const std::vector<Value> &arguments)
    {
        switch (function.id)
        {
        case FunctionId::Abs:
            if (arguments.size() != 1)
            {
                throw std::runtime_error(
                    "ABS requires exactly one argument");
            }
            return evaluateAbs(arguments[0]);

        default:
            throw std::runtime_error("Scalar function is not supported yet");
        }
    }

    Value evaluateUnary(const BoundUnaryExpr &unary, const Row &row)
    {
        Value operand = evaluateValue(*unary.operand, row);
        if (std::holds_alternative<std::monostate>(operand))
        {
            return std::monostate{};
        }

        if (unary.op == UnaryOperator::Not)
        {
            return !asBoolean(operand, "NOT");
        }
        if (unary.op == UnaryOperator::Positive)
        {
            return operand;
        }

        switch (unary.type())
        {
        case DataType::Int:
        {
            const std::int32_t value = std::get<std::int32_t>(operand);
            if (value == std::numeric_limits<std::int32_t>::min())
            {
                throw std::overflow_error("Integer negation overflow");
            }
            return static_cast<std::int32_t>(-value);
        }
        case DataType::BigInt:
        {
            const std::int64_t value = std::get<std::int64_t>(operand);
            if (value == std::numeric_limits<std::int64_t>::min())
            {
                throw std::overflow_error("BIGINT negation overflow");
            }
            return -value;
        }
        case DataType::Float:
            return static_cast<std::float32_t>(
                -std::get<std::float32_t>(operand));
        case DataType::Double:
            return static_cast<std::float64_t>(
                -std::get<std::float64_t>(operand));
        case DataType::Decimal:
            return negateDecimal(std::get<DecimalValue>(operand));
        default:
            throw std::runtime_error("Unary '-' requires a numeric operand");
        }
    }
}

Value evaluateValue(const BoundExpr &expr, const Row &row)
{
    switch (expr.kind())
    {
    case BoundExprKind::Literal:
        return static_cast<const BoundLiteralExpr &>(expr).value;

    case BoundExprKind::ColumnReference:
    {
        const auto &column = static_cast<const BoundColumnExpr &>(expr);
        if (column.columnIndex >= row.values.size())
        {
            throw std::runtime_error(
                "Column reference is outside the physical row");
        }
        return row.values[column.columnIndex];
    }

    case BoundExprKind::FunctionCall:
    {
        const auto &function = static_cast<const BoundFunctionCall &>(expr);
        if (function.category == FunctionCategory::Aggregate)
        {
            throw std::runtime_error(
                "Aggregate function cannot be evaluated per row");
        }

        std::vector<Value> arguments;
        arguments.reserve(function.arguments.size());
        for (const auto &argument : function.arguments)
        {
            arguments.push_back(evaluateValue(*argument, row));
        }
        return evaluateScalarFunction(function, arguments);
    }

    case BoundExprKind::Binary:
    {
        const auto &binary = static_cast<const BoundBinaryExpr &>(expr);
        Value left = evaluateValue(*binary.left, row);

        if (binary.op == BinaryOperator::And)
        {
            if (!asBoolean(left, "AND"))
            {
                return false;
            }
            return asBoolean(
                evaluateValue(*binary.right, row),
                "AND");
        }
        if (binary.op == BinaryOperator::Or)
        {
            if (asBoolean(left, "OR"))
            {
                return true;
            }
            return asBoolean(
                evaluateValue(*binary.right, row),
                "OR");
        }

        Value right = evaluateValue(*binary.right, row);
        switch (binary.op)
        {
        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
        case BinaryOperator::Multiply:
        case BinaryOperator::Divide:
            return evaluateArithmetic(
                binary.op,
                left,
                right,
                binary.type());

        case BinaryOperator::Eq:
        case BinaryOperator::Ne:
        case BinaryOperator::Gt:
        case BinaryOperator::Ge:
        case BinaryOperator::Lt:
        case BinaryOperator::Le:
            return compareValues(binary.op, left, right);

        case BinaryOperator::And:
        case BinaryOperator::Or:
            break;
        }
        throw std::runtime_error("Unsupported binary operator");
    }

    case BoundExprKind::Unary:
        return evaluateUnary(static_cast<const BoundUnaryExpr &>(expr), row);

    case BoundExprKind::IsNull:
    {
        const auto &isNull = static_cast<const BoundIsNullExpr &>(expr);
        const bool result = std::holds_alternative<std::monostate>(
            evaluateValue(*isNull.operand, row));
        return isNull.negated ? !result : result;
    }
    }

    throw std::runtime_error("Unknown bound expression kind");
}

bool evaluatePredicate(const BoundExpr &expr, const Row &row)
{
    return asBoolean(evaluateValue(expr, row), "WHERE");
}
