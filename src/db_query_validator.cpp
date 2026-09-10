#include "db_query_validator.h"
#include "db_decimal.h"

#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <string>

namespace
{
    const NamedTableRef &requireNamedTableRef(const TableRef &tableRef)
    {
        if (const auto *namedTable = dynamic_cast<const NamedTableRef *>(&tableRef))
        {
            return *namedTable;
        }

        throw std::runtime_error("Only single-table queries are supported");
    }



    bool iequals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        return std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
            return std::tolower(static_cast<unsigned char>(c1)) == 
                std::tolower(static_cast<unsigned char>(c2));
        });
    }

    std::optional<FunctionId> resolveFunctionName(
    std::string_view name)
    {
        if (iequals(name, "COUNT"))
            return FunctionId::Count;

        if (iequals(name, "SUM"))
            return FunctionId::Sum;

        if (iequals(name, "AVG"))
            return FunctionId::Avg;

        if (iequals(name, "MIN"))
            return FunctionId::Min;

        if (iequals(name, "MAX"))
            return FunctionId::Max;

        if (iequals(name, "ABS"))
            return FunctionId::Abs;

        return std::nullopt;
    }

    FunctionCategory resolveFunctionCategory(FunctionId id)
    {
        switch (id)
        {
        case FunctionId::Count:
        case FunctionId::Sum:
        case FunctionId::Avg:
        case FunctionId::Min:
        case FunctionId::Max:
            return FunctionCategory::Aggregate;

        case FunctionId::Abs:
        case FunctionId::Round:
        case FunctionId::Lower:
        case FunctionId::Upper:
            return FunctionCategory::Scalar;
        }

        throw std::runtime_error("Unknown function ID");
    }

    std::string resolveColumnName(
        const std::vector<std::string> &parts,
        const std::string_view &tableName)
    {
        if (parts.size() == 1)
        {
            return parts[0];
        }

        if (parts.size() == 2 && parts[0] == tableName)
        {
            return parts[1];
        }

        throw std::runtime_error("Unsupported column reference");
    }

    bool isNumericType(DataType type)
    {
        switch (type)
        {
        case DataType::Int:
        case DataType::Double:
        case DataType::Float:
        case DataType::Decimal:
        case DataType::BigInt:
            return true;

        default:
            return false;
        }
    }

    DataType resolveNumericResultType(
        BinaryOperator op,
        DataType leftType,
        DataType rightType)
    {
        if (!isNumericType(leftType) ||
            !isNumericType(rightType))
        {
            throw std::runtime_error(
                "Arithmetic operands must be numeric");
        }

        if (op == BinaryOperator::Divide)
        {
            if (leftType == DataType::Double ||
                rightType == DataType::Double ||
                leftType == DataType::Float ||
                rightType == DataType::Float)
            {
                return DataType::Double;
            }

            if (leftType == DataType::Decimal ||
                rightType == DataType::Decimal)
            {
                return DataType::Decimal;
            }

            if (leftType == DataType::BigInt ||
                rightType == DataType::BigInt)
            {
                return DataType::BigInt;
            }

            return DataType::Int;
        }

        if (leftType == DataType::Double ||
            rightType == DataType::Double)
        {
            return DataType::Double;
        }

        if (leftType == DataType::Decimal ||
            rightType == DataType::Decimal)
        {
            return DataType::Decimal;
        }

        if (leftType == DataType::Float ||
            rightType == DataType::Float)
        {
            return DataType::Float;
        }

        if (leftType == DataType::BigInt ||
            rightType == DataType::BigInt)
        {
            return DataType::BigInt;
        }

        return DataType::Int;
    }

    bool canAssign(DataType source, DataType target)
    {
        if (source == target)
        {
            return true;
        }

        if (source == DataType::Null)
        {
            return true; // NULL can be assigned to any type
        }

        if (source == DataType::Int &&
            (target == DataType::BigInt ||
             target == DataType::Decimal ||
             target == DataType::Double))
        {
            return true;
        }

        if (source == DataType::BigInt &&
            (target == DataType::Decimal || target == DataType::Double))
        {
            return true;
        }

        if (source == DataType::Decimal && target == DataType::Double)
        {
            return true;
        }

        return false;
    }

    DataType resolveBinaryResultType(
        BinaryOperator op,
        DataType leftType,
        DataType rightType)
    {
        switch (op)
        {
        case BinaryOperator::Eq:
        case BinaryOperator::Ne:
        case BinaryOperator::Lt:
        case BinaryOperator::Le:
        case BinaryOperator::Gt:
        case BinaryOperator::Ge:
        case BinaryOperator::And:
        case BinaryOperator::Or:
            return DataType::Boolean;

        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
        case BinaryOperator::Multiply:
        case BinaryOperator::Divide:
            return resolveNumericResultType(op,
                                            leftType,
                                            rightType);
        }

        throw std::runtime_error(
            "Unsupported binary operator");
    }

    DataType resolveUnaryResultType(
        UnaryOperator op,
        DataType operandType)
    {
        switch (op)
        {
        case UnaryOperator::Positive:
        case UnaryOperator::Negate:
        {
            if (!isNumericType(operandType))
            {
                throw std::runtime_error(
                    "Unary '+' and '-' require a numeric operand");
            }

            return operandType;
        }

        case UnaryOperator::Not:
        {
            if (operandType != DataType::Boolean &&
                operandType != DataType::Null)
            {
                throw std::runtime_error(
                    "NOT requires a boolean operand");
            }

            return DataType::Boolean;
        }
        }

        throw std::runtime_error("Unsupported unary operator");
    }

    std::string_view constraintTypeToString(ConstraintType type)
    {
        switch (type)
        {
        case ConstraintType::NotNull:
            return "NN";

        case ConstraintType::Null:
            return "NL";

        case ConstraintType::Default:
            return "DF";

        case ConstraintType::PrimaryKey:
            return "PK";

        case ConstraintType::Unique:
            return "UK";

        case ConstraintType::Check:
            return "CK";
        case ConstraintType::ForeignKey:
            return "FK";
        }

        return "UNKNOWN";
    }

    std::string join(
        const std::vector<std::string> &values,
        std::string_view separator)
    {
        std::string result;

        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
            {
                result += separator;
            }

            result += values[i];
        }

        return result;
    }

    std::string binaryOperatorText(BinaryOperator op)
    {
        switch (op)
        {
        case BinaryOperator::Add:
            return "+";
        case BinaryOperator::Subtract:
            return "-";
        case BinaryOperator::Multiply:
            return "*";
        case BinaryOperator::Divide:
            return "/";
        case BinaryOperator::And:
            return "AND";
        case BinaryOperator::Or:
            return "OR";
        case BinaryOperator::Eq:
            return "=";
        case BinaryOperator::Ne:
            return "<>";
        case BinaryOperator::Gt:
            return ">";
        case BinaryOperator::Ge:
            return ">=";
        case BinaryOperator::Lt:
            return "<";
        case BinaryOperator::Le:
            return "<=";
        }

        throw std::runtime_error("Unknown binary operator");
    }

    int expressionPrecedence(const Expr &expr)
    {
        const auto *binary = dynamic_cast<const BinaryExpr *>(&expr);
        if (!binary)
        {
            return dynamic_cast<const UnaryExpr *>(&expr) ? 80 : 100;
        }

        switch (binary->op)
        {
        case BinaryOperator::Or:
            return 30;
        case BinaryOperator::And:
            return 40;
        case BinaryOperator::Eq:
        case BinaryOperator::Ne:
        case BinaryOperator::Gt:
        case BinaryOperator::Ge:
        case BinaryOperator::Lt:
        case BinaryOperator::Le:
            return 50;
        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
            return 60;
        case BinaryOperator::Multiply:
        case BinaryOperator::Divide:
            return 70;
        }

        throw std::runtime_error("Unknown binary operator");
    }

    std::string formatNumber(const NumberValue &number)
    {
        return std::visit(
            [](const auto &value) -> std::string
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DecimalLiteral>)
                {
                    return value.text;
                }
                else if constexpr (std::is_integral_v<T>)
                {
                    return std::to_string(value);
                }
                else
                {
                    std::ostringstream stream;
                    stream << value;
                    return stream.str();
                }
            },
            number);
    }

    std::string quoteString(std::string_view value)
    {
        std::string result{"'"};
        for (char character : value)
        {
            result += character;
            if (character == '\'')
            {
                result += '\'';
            }
        }
        result += '\'';
        return result;
    }

    std::string upper(std::string value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::toupper(character));
            });
        return value;
    }

    std::string formatExpression(
        const Expr &expr,
        int parentPrecedence = 0,
        bool rightOperand = false)
    {
        if (const auto *column = dynamic_cast<const ColumnExpr *>(&expr))
        {
            return join(column->parts, ".");
        }

        if (const auto *number = dynamic_cast<const NumberExpr *>(&expr))
        {
            return formatNumber(number->value);
        }

        if (const auto *string = dynamic_cast<const StringExpr *>(&expr))
        {
            return quoteString(string->value);
        }

        if (const auto *boolean = dynamic_cast<const BooleanExpr *>(&expr))
        {
            return boolean->value ? "TRUE" : "FALSE";
        }

        if (dynamic_cast<const NullExpr *>(&expr))
        {
            return "NULL";
        }

        if (const auto *function = dynamic_cast<const FunctionCallExpr *>(&expr))
        {
            std::string result = upper(function->name) + "(";
            if (function->starArgument)
            {
                result += "*";
            }
            else
            {
                for (std::size_t i = 0; i < function->arguments.size(); ++i)
                {
                    if (i != 0)
                    {
                        result += ", ";
                    }
                    result += formatExpression(*function->arguments[i]);
                }
            }
            return result + ")";
        }

        if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr))
        {
            const std::string operatorText =
                unary->op == UnaryOperator::Not
                    ? "NOT "
                    : unary->op == UnaryOperator::Negate ? "-" : "+";
            return operatorText + formatExpression(*unary->operand, 80);
        }

        if (const auto *isNull = dynamic_cast<const IsNullExpr *>(&expr))
        {
            return formatExpression(*isNull->operand, 50) +
                   (isNull->negated ? " IS NOT NULL" : " IS NULL");
        }

        if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr))
        {
            const int precedence = expressionPrecedence(expr);
            std::string result =
                formatExpression(*binary->left, precedence) + " " +
                binaryOperatorText(binary->op) + " " +
                formatExpression(*binary->right, precedence, true);

            if (precedence < parentPrecedence ||
                (rightOperand && precedence == parentPrecedence))
            {
                return "(" + result + ")";
            }
            return result;
        }

        throw std::runtime_error("Cannot format unsupported expression");
    }

    bool containsColumnReference(const BoundExpr &expr)
    {
        if (dynamic_cast<const BoundColumnExpr *>(&expr))
        {
            return true;
        }

        if (const auto *binary = dynamic_cast<const BoundBinaryExpr *>(&expr))
        {
            return containsColumnReference(*binary->left) ||
                   containsColumnReference(*binary->right);
        }

        if (const auto *unary = dynamic_cast<const BoundUnaryExpr *>(&expr))
        {
            return containsColumnReference(*unary->operand);
        }

        return false;
    }

    bool isArithmeticOperator(BinaryOperator op)
    {
        return op == BinaryOperator::Add ||
               op == BinaryOperator::Subtract ||
               op == BinaryOperator::Multiply ||
               op == BinaryOperator::Divide;
    }

    DecimalValue numberToDecimal(const NumberValue &number)
    {
        return std::visit(
            [](const auto &value) -> DecimalValue
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DecimalLiteral>)
                {
                    return parseDecimalLiteral(value.text);
                }
                else
                {
                    return decimalFromInt64(
                        static_cast<std::int64_t>(value));
                }
            },
            number);
    }

    std::int64_t numberToInt64Exact(const NumberValue &number)
    {
        return std::visit(
            [](const auto &value) -> std::int64_t
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DecimalLiteral>)
                {
                    const auto converted =
                        decimalToInt64Exact(parseDecimalLiteral(value.text));
                    if (!converted)
                    {
                        throw std::runtime_error(
                            "Expected an integer numeric value");
                    }
                    return *converted;
                }
                else
                {
                    return static_cast<std::int64_t>(value);
                }
            },
            number);
    }

    std::float64_t numberToFloat64(const NumberValue &number)
    {
        return std::visit(
            [](const auto &value) -> std::float64_t
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DecimalLiteral>)
                {
                    return decimalToFloat64(
                        parseDecimalLiteral(value.text));
                }
                else
                {
                    return static_cast<std::float64_t>(value);
                }
            },
            number);
    }

    std::optional<NumberValue> numberFromExpr(const Expr &expr)
    {
        if (const auto *number = dynamic_cast<const NumberExpr *>(&expr))
        {
            return number->value;
        }

        const auto *unary = dynamic_cast<const UnaryExpr *>(&expr);
        if (!unary || unary->op == UnaryOperator::Not)
        {
            return std::nullopt;
        }

        std::optional<NumberValue> operand = numberFromExpr(*unary->operand);
        if (!operand || unary->op == UnaryOperator::Positive)
        {
            return operand;
        }

        return std::visit(
            [](const auto &value) -> NumberValue
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DecimalLiteral>)
                {
                    return DecimalLiteral{
                        .text = decimalToString(
                            negateDecimal(parseDecimalLiteral(value.text)))};
                }
                else
                {
                    if (value == std::numeric_limits<T>::min())
                    {
                        throw std::overflow_error(
                            "Numeric literal negation overflow");
                    }
                    return static_cast<T>(-value);
                }
            },
            *operand);
    }

    DataType numberDataType(const NumberValue &number)
    {
        return std::visit(
            [](const auto &value)
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::int32_t>)
                {
                    return DataType::Int;
                }
                else if constexpr (std::is_same_v<T, std::int64_t>)
                {
                    return DataType::BigInt;
                }
                else if constexpr (std::is_same_v<T, std::float32_t>)
                {
                    return DataType::Float;
                }
                else if constexpr (std::is_same_v<T, std::float64_t>)
                {
                    return DataType::Double;
                }
                else
                {
                    return DataType::Decimal;
                }
            },
            number);
    }

    Value numberToValue(const NumberValue &number)
    {
        return std::visit(
            [](const auto &value) -> Value
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DecimalLiteral>)
                {
                    return Value{parseDecimalLiteral(value.text)};
                }
                else
                {
                    return Value{value};
                }
            },
            number);
    }

    std::int64_t valueToInt64(const Value &value)
    {
        if (const auto *integer = std::get_if<std::int64_t>(&value))
        {
            return *integer;
        }
        if (const auto *integer = std::get_if<std::int32_t>(&value))
        {
            return *integer;
        }
        throw std::runtime_error("Expected an integer value");
    }

    DecimalValue valueToDecimal(const Value &value)
    {
        if (const auto *decimal = std::get_if<DecimalValue>(&value))
        {
            return *decimal;
        }
        return decimalFromInt64(valueToInt64(value));
    }

    std::float64_t valueToFloat64(const Value &value)
    {
        if (const auto *floating = std::get_if<std::float64_t>(&value))
        {
            return *floating;
        }
        if (const auto *decimal = std::get_if<DecimalValue>(&value))
        {
            return decimalToFloat64(*decimal);
        }
        return static_cast<std::float64_t>(valueToInt64(value));
    }

    std::int64_t checkedIntegerAdd(
        std::int64_t left,
        std::int64_t right)
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

    std::int64_t checkedIntegerSubtract(
        std::int64_t left,
        std::int64_t right)
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

    std::int64_t checkedIntegerMultiply(
        std::int64_t left,
        std::int64_t right)
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
}

QueryValidator::QueryValidator(const Catalog &catalog)
    : catalog(catalog) {}

BoundQuery QueryValidator::validate(const Statement &statement)
{
    if (const auto *insert = dynamic_cast<const InsertStatement *>(&statement))
    {
        return validateInsert(*insert);
    }

    if (const auto *select = dynamic_cast<const SelectStatement *>(&statement))
    {
        return validateSelect(*select);
    }

    if (const auto *deleteStmt = dynamic_cast<const DeleteStatement *>(&statement))
    {
        return validateDelete(*deleteStmt);
    }

    if (const auto *createTable = dynamic_cast<const CreateTableStatement *>(&statement))
    {
        return bindCreateTable(*createTable);
    }

    throw std::runtime_error("Unsupported statement type");
}

BoundDelete QueryValidator::validateDelete(const DeleteStatement &statement)
{
    if (!statement.from)
    {
        throw std::runtime_error("DELETE requires a FROM clause");
    }

    const NamedTableRef &tableRef = requireNamedTableRef(*statement.from);

    if (!catalog.tableExists(tableRef.name))
    {
        throw std::runtime_error("Table does not exist: " + tableRef.name);
    }

    // HeaderPage schema = catalog.getTableHeader(tableRef.name);
    BindContext context = catalog.createBindContext(tableRef.name);

    std::unique_ptr<BoundExpr> where;

    if (statement.where)
    {
        where = bindExpr(*statement.where, context);
    }

    return BoundDelete{
        .tableName = tableRef.name,
        .where = std::move(where)};
}

BoundSelect QueryValidator::validateSelect(const SelectStatement &statement)
{
    if (!statement.from)
    {
        throw std::runtime_error("SELECT requires a FROM clause");
    }

    const NamedTableRef &tableRef = requireNamedTableRef(*statement.from);

    if (!catalog.tableExists(tableRef.name))
    {
        throw std::runtime_error("Table does not exist: " + tableRef.name);
    }

    // HeaderPage schema = catalog.getTableHeader(tableRef.name);
    BindContext context = catalog.createBindContext(tableRef.name);
    std::unique_ptr<BoundExpr> where;

    if (statement.where)
    {
        where = bindExpr(*statement.where, context);
    }

    std::vector<BoundSelectItem> projections;

    const auto addColumnProjection = [&projections](const Column &column)
    {
        projections.push_back(BoundSelectItem{
            .expr = std::make_unique<BoundColumnExpr>(
                column.columnIndex,
                column.type),
            .outputName = column.name});
    };

    for (const std::unique_ptr<SelectItem> &item : statement.selectList)
    {
        if (dynamic_cast<const WildcardSelectItem *>(item.get()))
        {
            for (const Column &column : context.columns)
            {
                addColumnProjection(column);
            }
            continue;
        }

        if (const auto *qualifiedWildcard =
                dynamic_cast<const QualifiedWildcardSelectItem *>(item.get()))
        {
            if (qualifiedWildcard->qualifierParts.size() != 1 ||
                qualifiedWildcard->qualifierParts[0] != tableRef.name)
            {
                throw std::runtime_error("Unknown table qualifier in wildcard");
            }

            for (const Column &column : context.columns)
            {
                addColumnProjection(column);
            }
            continue;
        }

        if (const auto *exprItem = dynamic_cast<const ExprSelectItem *>(item.get()))
        {
            const auto *columnExpr =
                dynamic_cast<const ColumnExpr *>(exprItem->expr.get());

            std::string outputName = exprItem->alias;
            if (outputName.empty() && columnExpr)
            {
                outputName = resolveColumnName(
                    columnExpr->parts,
                    tableRef.name);
            }
            else if (outputName.empty())
            {
                outputName = formatExpression(*exprItem->expr);
            }

            projections.push_back(BoundSelectItem{
                .expr = bindExpr(*exprItem->expr, context, true),
                .outputName = std::move(outputName)});
            continue;
        }

        throw std::runtime_error("Unsupported SELECT item");
    }

    return BoundSelect{
        .tableName = tableRef.name,
        .projections = std::move(projections),
        .where = std::move(where),
        .groupBy = {},
        .having = nullptr};
}

Column QueryValidator::bindColumnDefinition(
    const ColumnDefExpr &colDef,
    std::uint32_t columnIndex)
{
    Column column;
    column.name = colDef.columnName;
    column.type = colDef.dataType->type;
    column.nullable = true; // Default to nullable unless a NOT NULL constraint is found
    column.columnIndex = columnIndex;

    return column;
}

BoundCreateTable QueryValidator::bindCreateTable(
    const CreateTableStatement &statement)
{
    BoundCreateTable result;
    result.tableName = statement.tableName;

    // First pass: build all columns.
    for (const auto &columnDef : statement.columns)
    {
        result.columns.push_back(
            bindColumnDefinition(
                *columnDef,
                result.columns.size()));
    }

    BindContext context{
        .tableName = result.tableName,
        .columns = result.columns};

    // Second pass: bind constraints.
    for (const auto &constraint : statement.constraints)
    {
        result.constraints.push_back(
            bindConstraintExpr(*constraint, context));
    }

    result.columns = std::move(context.columns);

    return result;
}

Constraint QueryValidator::bindConstraintExpr(
    const ConstraintExpr &expr, BindContext &context)

{
    std::string constraintName;

    switch (expr.constraintType)
    {
    case ConstraintType::NotNull:
    {
        const auto &notNullExpr =
            dynamic_cast<const NotNullConstraintExpr &>(expr);
        Column &column = context.resolveColumn(notNullExpr.columnName);
        column.nullable = false;
        constraintName = expr.constraintName.value_or(
            std::format("{}_{}", constraintTypeToString(notNullExpr.constraintType), notNullExpr.columnName)); // Use a format for the constraint name if not provided

        return BoundNotNullConstraintExpr{
            std::move(constraintName),
            column.columnIndex};
    }

    case ConstraintType::Null:
    {
        const auto &nullExpr =
            dynamic_cast<const NullConstraintExpr &>(expr);
        Column &column = context.resolveColumn(nullExpr.columnName);
        column.nullable = true;
        constraintName = expr.constraintName.value_or(
            std::format(
                "{}_{}",
                constraintTypeToString(nullExpr.constraintType),
                nullExpr.columnName));

        return BoundNullConstraintExpr{
            std::move(constraintName),
            column.columnIndex};
    }

    case ConstraintType::Default:
    {
        const auto &defaultExpr =
            dynamic_cast<const DefaultConstraintExpr &>(expr);

        Column &column = context.resolveColumn(defaultExpr.columnName);
        constraintName = expr.constraintName.value_or(
            std::format("{}_{}", constraintTypeToString(defaultExpr.constraintType), defaultExpr.columnName)); // Use a format for the constraint name if not provided

        auto boundValue = bindExpr(*defaultExpr.value, context);
        if (containsColumnReference(*boundValue))
        {
            throw std::runtime_error(
                "DEFAULT expressions cannot reference table columns");
        }
        if (!canAssign(boundValue->type(), column.type))
        {
            throw std::runtime_error(
                std::format(
                    "Default value type is incompatible with column '{}'",
                    column.name));
        }

        return BoundDefaultConstraintExpr{
            std::move(constraintName),
            std::move(boundValue),
            column.columnIndex};
    }

    case ConstraintType::PrimaryKey:
    {
        const auto &pkExpr =
            dynamic_cast<const PrimaryKeyConstraintExpr &>(expr);

        constraintName = expr.constraintName.value_or(
            std::format(
                "{}_{}",
                constraintTypeToString(pkExpr.constraintType),
                join(pkExpr.columns, "_")));

        std::vector<ColumnId> columnIds;
        columnIds.reserve(pkExpr.columns.size());

        std::unordered_set<ColumnId> seenColumns;

        for (const std::string &columnName : pkExpr.columns)
        {
            Column &column = context.resolveColumn(columnName);

            if (!seenColumns.insert(column.columnIndex).second)
            {
                throw std::runtime_error(
                    std::format(
                        "Column '{}' appears more than once in primary key",
                        columnName));
            }

            column.nullable = false;
            columnIds.push_back(column.columnIndex);
        }

        return BoundPrimaryKeyConstraintExpr{
            std::move(constraintName),
            std::move(columnIds)};
    }

    case ConstraintType::ForeignKey:
    {
        const auto &fkExpr = dynamic_cast<const ForeignKeyConstraintExpr &>(expr);

        std::vector<ColumnId> localColumnIds;
        localColumnIds.reserve(fkExpr.localColumns.size());

        for (const std::string &name : fkExpr.localColumns)
        {
            const Column &column = context.resolveColumn(name);
            localColumnIds.push_back(column.columnIndex);
        }

        std::vector<ColumnId> referencedColumnIds;
        BindContext referencedTable = catalog.createBindContext(fkExpr.referencedTable);
        referencedColumnIds.reserve(
            fkExpr.referencedColumns.size());

        for (const std::string &name :
             fkExpr.referencedColumns)
        {
            const Column &column =
                referencedTable.resolveColumn(name);

            referencedColumnIds.push_back(column.columnIndex);
        }
        if (localColumnIds.size() != referencedColumnIds.size())
        {
            throw std::runtime_error(
                "Foreign key column count does not match referenced column count");
        }
        for (std::size_t i = 0; i < localColumnIds.size(); ++i)
        {
            const Column &local =
                context.resolveColumn(localColumnIds[i]);

            const Column &referenced =
                referencedTable.resolveColumn(
                    referencedColumnIds[i]);

            if (local.type != referenced.type)
            {
                throw std::runtime_error(
                    "Foreign key column types are incompatible");
            }
        }
        constraintName = expr.constraintName.value_or(
            std::format(
                "{}_{}_{}",
                constraintTypeToString(fkExpr.constraintType),
                join(fkExpr.localColumns, "_"),
                join(fkExpr.referencedColumns, "_")));
        return BoundForeignKeyConstraintExpr{
            std::move(constraintName),
            std::move(localColumnIds),
            std::string{referencedTable.tableName},
            std::move(referencedColumnIds)};
    }

    case ConstraintType::Unique:
    {
        const auto &uniqueExpr =
            dynamic_cast<const UniqueConstraintExpr &>(expr);

        constraintName = expr.constraintName.value_or(
            std::format(
                "{}_{}",
                constraintTypeToString(uniqueExpr.constraintType),
                join(uniqueExpr.columns, "_")));

        std::vector<ColumnId> boundColumns;
        boundColumns.reserve(uniqueExpr.columns.size());

        for (const std::string &columnName : uniqueExpr.columns)
        {
            const Column &column = context.resolveColumn(columnName);
            boundColumns.push_back(column.columnIndex);
        }

        return BoundUniqueConstraintExpr{
            std::move(constraintName),
            std::move(boundColumns)};
    }

    case ConstraintType::Check:
    {
        const auto &checkExpr =
            dynamic_cast<const CheckConstraintExpr &>(expr);
        auto condition = bindExpr(*checkExpr.condition, context);

        if (condition->type() != DataType::Boolean)
        {
            throw std::runtime_error("CHECK condition must be boolean");
        }

        constraintName = expr.constraintName.value_or("CK");
        return BoundCheckConstraintExpr{
            std::move(constraintName),
            std::move(condition)};
    }

    default:
        throw std::runtime_error("Unsupported constraint type");
    }
}

BoundInsert QueryValidator::validateInsert(const InsertStatement &statement)
{
    if (!catalog.tableExists(statement.tableName))
    {
        throw std::runtime_error("Table does not exist: " + statement.tableName);
    }

    // HeaderPage schema = catalog.getTableHeader(statement.tableName);
    BindContext context = catalog.createBindContext(statement.tableName);

    std::vector<const Column *> targetColumns;
    targetColumns.reserve(
        statement.columns.empty()
            ? context.columns.size()
            : statement.columns.size());

    if (statement.columns.empty())
    {
        for (const Column &column : context.columns)
        {
            targetColumns.push_back(&column);
        }
    }
    else
    {
        for (const std::string &columnName : statement.columns)
        {
            targetColumns.push_back(&context.resolveColumn(columnName));
        }
    }
    std::vector<Row> rows;
    rows.reserve(statement.valuesList.size());
    for (const std::vector<std::unique_ptr<Expr>> &rowValues : statement.valuesList)
    {

        if (targetColumns.size() != rowValues.size())
        {
            throw std::runtime_error("INSERT column count does not match value count");
        }

        Row row{
            .values = std::vector<Value>(context.columns.size(), std::monostate{})};

        for (std::size_t i = 0; i < targetColumns.size(); ++i)
        {
            const Column &column = *targetColumns[i];
            const Expr &expr = *rowValues[i];
            row.values[column.columnIndex] = bindLiteralValue(expr, column);
        }

        for (const Column &column : context.columns)
        {
            if (!column.nullable &&
                std::holds_alternative<std::monostate>(row.values[column.columnIndex]))
            {
                throw std::runtime_error(
                    "Missing value for NOT NULL column: " + column.name);
            }
        }
        rows.push_back(std::move(row));
    }

    return BoundInsert{
        .tableName = statement.tableName,
        .rows = std::move(rows)};
}

Value QueryValidator::bindLiteralValue(
    const Expr &expr,
    const Column &targetColumn) const
{
    if (dynamic_cast<const NullExpr *>(&expr))
    {
        if (!targetColumn.nullable)
        {
            throw std::runtime_error(
                "NULL provided for NOT NULL column: " + targetColumn.name);
        }

        return std::monostate{};
    }

    const std::optional<NumberValue> number = numberFromExpr(expr);

    switch (targetColumn.type)
    {
    case DataType::Int:
    {
        if (!number)
        {
            throw std::runtime_error(
                "Expected numeric value for column: " + targetColumn.name);
        }

        const std::int64_t value = numberToInt64Exact(*number);
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max())
        {
            throw std::runtime_error(
                "Integer value is out of range for column: " +
                targetColumn.name);
        }

        return static_cast<std::int32_t>(value);
    }

    case DataType::BigInt:
    {
        if (!number)
        {
            throw std::runtime_error(
                "Expected numeric value for column: " + targetColumn.name);
        }
        return numberToInt64Exact(*number);
    }

    case DataType::Decimal:
    {
        if (!number)
        {
            throw std::runtime_error(
                "Expected numeric value for column: " + targetColumn.name);
        }
        return numberToDecimal(*number);
    }

    case DataType::Double:
    case DataType::Float:
    {
        if (!number)
        {
            throw std::runtime_error(
                "Expected numeric value for column: " + targetColumn.name);
        }
        return numberToFloat64(*number);
    }

    case DataType::Text:
    {
        const auto *string = dynamic_cast<const StringExpr *>(&expr);

        if (!string)
        {
            throw std::runtime_error(
                "Expected string value for column: " + targetColumn.name);
        }

        return string->value;
    }

    case DataType::Null:
        throw std::runtime_error("Cannot bind non-null value to NULL column");

    case DataType::Boolean:
    {
        const auto *boolExpr = dynamic_cast<const BooleanExpr *>(&expr);
        if (!boolExpr)
        {
            throw std::runtime_error(
                "Expected boolean value for column: " + targetColumn.name);
        }
        return boolExpr->value;
    }

    default:
        throw std::runtime_error("Unsupported target column type");
    }
}

Value evaluateConstantBinary(
    BinaryOperator op,
    const Value &left,
    const Value &right,
    DataType resultType)
{
    if (resultType == DataType::Decimal)
    {
        const DecimalValue lhs = valueToDecimal(left);
        const DecimalValue rhs = valueToDecimal(right);
        switch (op)
        {
        case BinaryOperator::Add:
            return addDecimals(lhs, rhs);
        case BinaryOperator::Subtract:
            return subtractDecimals(lhs, rhs);
        case BinaryOperator::Multiply:
            return multiplyDecimals(lhs, rhs);
        case BinaryOperator::Divide:
            return divideDecimals(lhs, rhs);
        default:
            break;
        }
    }

    if (resultType == DataType::Double)
    {
        const std::float64_t lhs = valueToFloat64(left);
        const std::float64_t rhs = valueToFloat64(right);
        switch (op)
        {
        case BinaryOperator::Add:
            return Value{lhs + rhs};
        case BinaryOperator::Subtract:
            return Value{lhs - rhs};
        case BinaryOperator::Multiply:
            return Value{lhs * rhs};
        case BinaryOperator::Divide:
            if (rhs == 0)
            {
                throw std::runtime_error(
                    "Division by zero in constant expression");
            }
            return Value{lhs / rhs};
        default:
            break;
        }
    }

    if (resultType == DataType::Int || resultType == DataType::BigInt)
    {
        const std::int64_t lhs = valueToInt64(left);
        const std::int64_t rhs = valueToInt64(right);
        std::int64_t result;

        switch (op)
        {
        case BinaryOperator::Add:
            result = checkedIntegerAdd(lhs, rhs);
            break;
        case BinaryOperator::Subtract:
            result = checkedIntegerSubtract(lhs, rhs);
            break;
        case BinaryOperator::Multiply:
            result = checkedIntegerMultiply(lhs, rhs);
            break;
        case BinaryOperator::Divide:
            if (rhs == 0)
            {
                throw std::runtime_error(
                    "Division by zero in constant expression");
            }
            if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1)
            {
                throw std::overflow_error("Integer expression overflow");
            }
            result = lhs / rhs;
            break;
        default:
            throw std::runtime_error(
                "Operator is not a constant arithmetic operator");
        }

        if (resultType == DataType::Int)
        {
            if (result < std::numeric_limits<std::int32_t>::min() ||
                result > std::numeric_limits<std::int32_t>::max())
            {
                throw std::overflow_error("Integer expression overflow");
            }
            return Value{static_cast<std::int32_t>(result)};
        }
        return Value{result};
    }

    throw std::runtime_error(
        "Unsupported constant arithmetic result type");
}

std::unique_ptr<BoundExpr> QueryValidator::bindExpr(
    const Expr &expr,
    const BindContext &context,
    bool allowAggregates) const
{
    if (const auto *column =
            dynamic_cast<const ColumnExpr *>(&expr))
    {
        std::string_view view = context.tableName;

        const std::string str{view};
        std::string columnName = resolveColumnName(
            column->parts,
            context.tableName);

        const Column &resolved =
            context.resolveColumn(columnName);

        return std::make_unique<BoundColumnExpr>(
            resolved.columnIndex,
            resolved.type);
    }

    if (const auto *number = dynamic_cast<const NumberExpr *>(&expr))
    {
        return std::make_unique<BoundLiteralExpr>(
            numberToValue(number->value),
            numberDataType(number->value));
    }

    if (const auto *string = dynamic_cast<const StringExpr *>(&expr))
    {
        return std::make_unique<BoundLiteralExpr>(
            Value{string->value},
            DataType::Text);
    }

    if (const auto *boolean = dynamic_cast<const BooleanExpr *>(&expr))
    {
        return std::make_unique<BoundLiteralExpr>(
            Value{boolean->value},
            DataType::Boolean);
    }



    if (dynamic_cast<const NullExpr *>(&expr))
    {
        return std::make_unique<BoundLiteralExpr>(
            Value{std::monostate{}},
            DataType::Null);
    }





    if (const auto *functionCall = dynamic_cast<const FunctionCallExpr *>(&expr))
    {
        auto id = resolveFunctionName(functionCall->name);
        if (!id)
        {
            throw std::runtime_error("Unknown function: " + functionCall->name);
        }
        auto category = resolveFunctionCategory(*id);

        if (category == FunctionCategory::Aggregate && !allowAggregates)
        {
            throw std::runtime_error(
                "Aggregate functions are not allowed in this expression");
        }

        if (functionCall->starArgument && *id != FunctionId::Count)
        {
            throw std::runtime_error(
                "Only COUNT accepts '*' as an argument");
        }

        std::vector<std::unique_ptr<BoundExpr>> boundList;
        boundList.reserve(functionCall->arguments.size());

        for (const auto &item : functionCall->arguments)
        {
            boundList.push_back(bindExpr(
                *item,
                context,
                allowAggregates && category != FunctionCategory::Aggregate));
        }

        DataType resultType;
        switch (*id)
        {
        case FunctionId::Count:
            if (functionCall->starArgument ? !boundList.empty() : boundList.size() != 1)
            {
                throw std::runtime_error("COUNT requires '*' or exactly one argument");
            }
            resultType = DataType::BigInt;
            break;

        case FunctionId::Sum:
            if (boundList.size() != 1 ||
                (!isNumericType(boundList[0]->type()) &&
                 boundList[0]->type() != DataType::Null))
            {
                throw std::runtime_error("SUM requires exactly one numeric argument");
            }
            resultType = boundList[0]->type();
            if (resultType == DataType::Int)
            {
                resultType = DataType::BigInt;
            }
            else if (resultType == DataType::Float)
            {
                resultType = DataType::Double;
            }
            break;

        case FunctionId::Abs:
            if (boundList.size() != 1 ||
                !isNumericType(boundList[0]->type()))
            {
                throw std::runtime_error(
                    "ABS requires exactly one numeric argument");
            }
            resultType = boundList[0]->type();
            break;

        default:
            throw std::runtime_error(
                "Function is not supported yet: " + functionCall->name);
        }

        auto boundFunction = std::make_unique<BoundFunctionCall>(
            *id,
            category,
            std::move(boundList),
            resultType);
        boundFunction->starArgument = functionCall->starArgument;
        return boundFunction;
    }

    if (const auto *isNull = dynamic_cast<const IsNullExpr *>(&expr))
    {
        return std::make_unique<BoundIsNullExpr>(
            bindExpr(*isNull->operand, context, allowAggregates),
            isNull->negated);
    }

    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr))
    {
        auto leftBound = bindExpr(*binary->left, context, allowAggregates);
        auto rightBound = bindExpr(*binary->right, context, allowAggregates);
        DataType resultType = resolveBinaryResultType(
            binary->op,
            leftBound->type(),
            rightBound->type());

        if (isArithmeticOperator(binary->op) &&
            leftBound->kind() == BoundExprKind::Literal &&
            rightBound->kind() == BoundExprKind::Literal)
        {
            const auto &leftLiteral =
                static_cast<const BoundLiteralExpr &>(*leftBound);

            const auto &rightLiteral =
                static_cast<const BoundLiteralExpr &>(*rightBound);

            Value result = evaluateConstantBinary(
                binary->op,
                leftLiteral.value,
                rightLiteral.value,
                resultType);

            return std::make_unique<BoundLiteralExpr>(
                std::move(result),
                resultType);
        }

        return std::make_unique<BoundBinaryExpr>(
            binary->op,
            std::move(leftBound),
            std::move(rightBound),
            resultType);
    }

    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr))
    {
        auto operand = bindExpr(
            *unary->operand,
            context,
            allowAggregates);

        DataType resultType = resolveUnaryResultType(
            unary->op,
            operand->type());
        if (operand->kind() == BoundExprKind::Literal)
        {
            const auto &literal =
                static_cast<const BoundLiteralExpr &>(*operand);

            if (unary->op == UnaryOperator::Positive)
            {
                return std::make_unique<BoundLiteralExpr>(
                    literal.value,
                    resultType);
            }

            if (unary->op == UnaryOperator::Negate)
            {
                if (resultType == DataType::Int)
                {
                    const std::int32_t value =
                        std::get<std::int32_t>(literal.value);

                    if (value == std::numeric_limits<std::int32_t>::min())
                    {
                        throw std::overflow_error(
                            "Integer negation overflow");
                    }

                    return std::make_unique<BoundLiteralExpr>(
                        Value{-value},
                        DataType::Int);
                }

                if (resultType == DataType::BigInt)
                {
                    const std::int64_t value =
                        std::get<std::int64_t>(literal.value);

                    if (value == std::numeric_limits<std::int64_t>::min())
                    {
                        throw std::overflow_error(
                            "BIGINT negation overflow");
                    }

                    return std::make_unique<BoundLiteralExpr>(
                        Value{-value},
                        DataType::BigInt);
                }

                if (resultType == DataType::Decimal)
                {
                    return std::make_unique<BoundLiteralExpr>(
                        Value{negateDecimal(
                            std::get<DecimalValue>(literal.value))},
                        DataType::Decimal);
                }

                if (resultType == DataType::Double)
                {
                    return std::make_unique<BoundLiteralExpr>(
                        Value{-std::get<std::float64_t>(literal.value)},
                        DataType::Double);
                }
            }
        }

        return std::make_unique<BoundUnaryExpr>(
            unary->op,
            std::move(operand),
            resultType);
    }

    throw std::runtime_error("Unsupported expression");
}
