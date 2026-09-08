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
    template <typename T>
    NumberValue applyArithmeticTyped(
        BinaryOperator op,
        const NumberValue& left,
        const NumberValue& right)
    {
        if (!std::holds_alternative<T>(left) ||
            !std::holds_alternative<T>(right))
        {
            throw std::runtime_error(
                "Bound arithmetic operands do not match resolved type");
        }

        const T lhs = std::get<T>(left);
        const T rhs = std::get<T>(right);

        switch (op)
        {
            case BinaryOperator::Add:
                return NumberValue{lhs + rhs};

            case BinaryOperator::Subtract:
                return NumberValue{lhs - rhs};

            case BinaryOperator::Multiply:
                return NumberValue{lhs * rhs};

            case BinaryOperator::Divide:
            {
                if (rhs == T{0})
                {
                    throw std::runtime_error(
                        "Division by zero");
                }

                return NumberValue{lhs / rhs};
            }

            default:
                throw std::runtime_error(
                    "Unsupported arithmetic operator");
        }
    }

        
    NumberValue applyArithmeticOp(
        BinaryOperator op,
        const NumberValue& left,
        const NumberValue& right,
        DataType type)
    {
        switch (type)
        {
            case DataType::Int:
                return applyArithmeticTyped<std::int32_t>(
                    op, left, right);

            case DataType::BigInt:
                return applyArithmeticTyped<std::int64_t>(
                    op, left, right);

            case DataType::Float:
                return applyArithmeticTyped<std::float32_t>(
                    op, left, right);

            case DataType::Double:
                return applyArithmeticTyped<std::float64_t>(
                    op, left, right);

            case DataType::Decimal:
            {
                const auto& lhs =
                    std::get<DecimalValue>(left);

                const auto& rhs =
                    std::get<DecimalValue>(right);

                switch (op)
                {
                    case BinaryOperator::Add:
                        return addDecimals(lhs, rhs);

                    case BinaryOperator::Subtract:
                        return subtractDecimals(lhs, rhs);

                    case BinaryOperator::Multiply:
                        return multiplyDecimals(lhs, rhs);

                    case BinaryOperator::Divide:
                    {
                        if (rhs.coefficient == 0)
                        {
                            throw std::runtime_error(
                                "Division by zero");
                        }

                        return divideDecimals(lhs, rhs);
                    }

                    default:
                        throw std::runtime_error(
                            "Unsupported arithmetic operator");
                }
            }

            default:
                throw std::runtime_error(
                    "Arithmetic requires numeric operands");
        }
    }

    bool isArithmeticOperator(BinaryOperator op)
    {
        return op == BinaryOperator::Add ||
               op == BinaryOperator::Subtract ||
               op == BinaryOperator::Multiply ||
               op == BinaryOperator::Divide;
    }

    bool isComparisonOperator(BinaryOperator op)
    {
        return op == BinaryOperator::Eq ||
               op == BinaryOperator::Ne ||
               op == BinaryOperator::Gt ||
               op == BinaryOperator::Ge ||
               op == BinaryOperator::Lt ||
               op == BinaryOperator::Le;
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
    Value evaluateAbs(const Value &value)
    {
        return std::visit(
            [](const auto &v) -> Value
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, DecimalValue>)
                {
                    return Value{decimalAbs(v)};
                }
                else if constexpr (std::is_arithmetic_v<T>)
                {
                    return Value{std::abs(v)};
                }
                else
                {
                    throw std::runtime_error("ABS function requires a numeric argument");
                }
            },
            value);
    }

    Value evaluateLower(const Value &value)
    {
        if (const auto *str = std::get_if<std::string>(&value))
        {
            std::string lowerStr = *str;
            for (char &c : lowerStr)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return Value{lowerStr};
        }
        else
        {
            throw std::runtime_error("LOWER function requires a string argument");
        }
    }

    Value evaluateUpper(const Value &value)
    {
        if (const auto *str = std::get_if<std::string>(&value))
        {
            std::string upperStr = *str;
            for (char &c : upperStr)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return Value{upperStr};
        }
        else
        {
            throw std::runtime_error("UPPER function requires a string argument");
        }
    }

    Value evaluateRound(const std::vector<Value> &args)
    {
        if (args.empty())
        {
            throw std::runtime_error("ROUND function requires at least one argument");
        }

        const Value &value = args[0];
        int scale = 0;

        if (args.size() > 1)
        {
            if (const auto *scaleValue = std::get_if<std::int32_t>(&args[1]))
            {
                scale = *scaleValue;
            }
            else
            {
                throw std::runtime_error("ROUND function's second argument must be an integer");
            }
        }

        return std::visit(
            [scale](const auto &v) -> Value
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, DecimalValue>)
                {
                    return Value{decimalRound(v, scale)};
                }
                else if constexpr (std::is_floating_point_v<T>)
                {
                    double factor = std::pow(10.0, scale);
                    return Value{static_cast<T>(std::round(v * factor) / factor)};
                }
                else
                {
                    throw std::runtime_error("ROUND function requires a numeric argument");
                }
            },
            value);
    }

    Value evaluateScalarFunction(
        FunctionId id,
        const std::vector<Value>& args)
    {
        switch (id)
        {
            case FunctionId::Abs:
                return evaluateAbs(args[0]);

            case FunctionId::Lower:
                return evaluateLower(args[0]);

            case FunctionId::Upper:
                return evaluateUpper(args[0]);

            case FunctionId::Round:
                return evaluateRound(args);

            default:
                throw std::runtime_error(
                    "Not a scalar function");
        }
    }

    Value evaluateValue(
        const BoundExpr& expr,
        const Row& row)
        {
        switch (expr.kind())
        {
            case BoundExprKind::Literal:
            {
                const auto& literal =
                    static_cast<const BoundLiteralExpr&>(expr);

                return literal.value;
            }

            case BoundExprKind::ColumnReference:
            {
                const auto& column =
                    static_cast<const BoundColumnExpr&>(expr);

                return row.values[column.columnIndex];
            }
            case BoundExprKind::FunctionCall:
            {
                const auto& function =
                    static_cast<const BoundFunctionCall&>(expr);

                if (function.category == FunctionCategory::Aggregate)
                {
                    throw std::runtime_error(
                        "Aggregate function cannot be evaluated per row");
                }

                std::vector<Value> args;
                args.reserve(function.arguments.size());

                for (const auto& arg : function.arguments)
                {
                    args.push_back(
                        evaluateValue(*arg, row));
                }

                return evaluateScalarFunction(
                    function.id,
                    args);
            }

            case BoundExprKind::Binary:
            {
                const auto& binary =
                    static_cast<const BoundBinaryExpr&>(expr);

                // Short-circuit AND
                if (binary.op == BinaryOperator::And)
                {
                    Value left =
                        evaluateValue(*binary.left, row);

                    bool lhs = std::get<bool>(left);

                    if (!lhs)
                    {
                        return Value{false};
                    }

                    Value right =
                        evaluateValue(*binary.right, row);

                    return Value{
                        lhs && std::get<bool>(right)
                    };
                }

                // Short-circuit OR
                if (binary.op == BinaryOperator::Or)
                {
                    Value left =
                        evaluateValue(*binary.left, row);

                    bool lhs = std::get<bool>(left);

                    if (lhs)
                    {
                        return Value{true};
                    }

                    Value right =
                        evaluateValue(*binary.right, row);

                    return Value{
                        lhs || std::get<bool>(right)
                    };
                }

                Value left =
                    evaluateValue(*binary.left, row);

                Value right =
                    evaluateValue(*binary.right, row);

                if (isArithmeticOperator(binary.op))
                {
                    return applyArithmeticOp(
                        binary.op,
                        left,
                        right,
                        binary.type());
                }

                if (isComparisonOperator(binary.op))
                {
                    return Value{
                        compareValues(
                            binary.op,
                            left,
                            right)
                    };
                }

                throw std::runtime_error(
                    "Unsupported binary operator");
            }

            default:
                throw std::runtime_error(
                    "Unsupported expression");
        }
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
            {
            Value leftValue = evaluateValue(*expr.left, row);
            Value rightValue = evaluateValue(*expr.right, row);
            return compareValues(expr.op, leftValue, rightValue);
            }
        }

        throw std::runtime_error("Unsupported binary predicate operator");
    }
}

bool evaluatePredicate(const BoundExpr &expr, const Row &row)
{
    switch (expr.kind())
    {
        case BoundExprKind::Literal:
        {
            const auto *literal = dynamic_cast<const BoundLiteralExpr *>(&expr);
  
            if (literal->value.index() != 6) // index of bool in Value variant
            {
                throw std::runtime_error(
                    "Literal expression does not evaluate to a boolean");
            }
            return std::get<bool>(literal->value);
        }
        // case BoundExprKind::ColumnReference:
        // {
        //     const auto *column = dynamic_cast<const BoundColumnExpr *>(&expr);
        //     Value value = row.values[column->columnIndex];
        //     if (value.index() != 6) // index of bool in Value variant
        //     {
        //         throw std::runtime_error(
        //             "Column reference does not evaluate to a boolean");
        //     }
        //     return std::get<bool>(value);
        // }
        case BoundExprKind::Binary:
        {
            const auto *binary =
                dynamic_cast<const BoundBinaryExpr *>(&expr);
            if (binary->op == BinaryOperator::And)
            {
                return evaluatePredicate(*binary->left, row) &&
                       evaluatePredicate(*binary->right, row);
            }
            if (binary->op == BinaryOperator::Or)
            {
                return evaluatePredicate(*binary->left, row) ||
                       evaluatePredicate(*binary->right, row);
            }
            return evaluatePredicate(*binary, row);
        }
    

        case BoundExprKind::Unary:
        {
            const auto *unary = dynamic_cast<const BoundUnaryExpr *>(&expr);
        
            if (unary->op == UnaryOperator::Not)
            {
                return !evaluatePredicate(*unary->operand, row);
            }
        }

        case BoundExprKind::IsNull:
        {
            const auto *isNull = dynamic_cast<const BoundIsNullExpr *>(&expr);
            Value value = evaluateValue(*isNull->operand, row);
            bool result = std::holds_alternative<std::monostate>(value);
            return isNull->negated ? !result : result;
        }
        default:{
            throw std::runtime_error("Expression does not evaluate to a predicate");}
}
}


bool evaluatePredicate(const BoundExpr &expr, const Row &row)
{
    Value result = evaluateValue(expr, row);

    if (!std::holds_alternative<bool>(result))
    {
        throw std::runtime_error(
            "WHERE expression did not produce a boolean");
    }

    return std::get<bool>(result);
}