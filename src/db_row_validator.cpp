#include "db_row_validator.h"

#include <format>
#include <string>
#include <variant>

namespace
{
    bool valueMatchesColumn(const Column &column, const Value &value)
    {
        if (std::holds_alternative<std::monostate>(value))
        {
            return column.nullable;
        }

        switch (column.type)
        {
        case DataType::Int:
            return std::holds_alternative<std::int32_t>(value);

        case DataType::BigInt:
            return std::holds_alternative<std::int64_t>(value);

        case DataType::Double:
            return std::holds_alternative<std::float64_t>(value);

        case DataType::Decimal:
            return std::holds_alternative<DecimalValue>(value);

        case DataType::Boolean:
            return std::holds_alternative<bool>(value);

        case DataType::Text:
            return std::holds_alternative<std::string>(value);

        case DataType::Null:
            return std::holds_alternative<std::monostate>(value);

        default:
            return false;
        }
    }

    std::string dataTypeName(DataType type)
    {
        switch (type)
        {
        case DataType::Null:
            return "null";
        case DataType::Int:
            return "int";
        case DataType::BigInt:
            return "bigint";
        case DataType::Double:
            return "double";
        case DataType::Decimal:
            return "decimal";
        case DataType::Boolean:
            return "boolean";
        case DataType::Text:
            return "text";
        default:
            return "unknown";
        }
    }

    std::string valueTypeName(const Value &value)
    {
        if (std::holds_alternative<std::monostate>(value))
        {
            return "null";
        }

        if (std::holds_alternative<std::int32_t>(value))
        {
            return "int";
        }

        if (std::holds_alternative<std::int64_t>(value))
        {
            return "bigint";
        }

        if (std::holds_alternative<std::float64_t>(value))
        {
            return "double";
        }

        if (std::holds_alternative<DecimalValue>(value))
        {
            return "decimal";
        }

        if (std::holds_alternative<bool>(value))
        {
            return "boolean";
        }

        if (std::holds_alternative<std::string>(value))
        {
            return "text";
        }

        return "unknown";
    }
}

RowValidationResult validateRowAgainstSchema(
    const std::vector<Column> &columns,
    const Row &row)
{
    if (columns.size() != row.values.size())
    {
        return RowValidationResult{
            .valid = false,
            .columnIndex = std::nullopt,
            .message = std::format(
                "row has {} values but schema expects {} columns",
                row.values.size(),
                columns.size())};
    }

    for (const Column &column : columns)
    {
        const Value &value = row.values[column.columnIndex];

        if (std::holds_alternative<std::monostate>(value) && !column.nullable)
        {
            return RowValidationResult{
                .valid = false,
                .columnIndex = column.columnIndex,
                .message = std::format(
                    "column '{}' does not allow null values",
                    column.name)};
        }

        if (!valueMatchesColumn(column, value))
        {
            return RowValidationResult{
                .valid = false,
                .columnIndex = column.columnIndex,
                .message = std::format(
                    "column '{}' expects type '{}' but row value has type '{}'",
                    column.name,
                    dataTypeName(column.type),
                    valueTypeName(value))};
        }
    }

    return RowValidationResult{
        .valid = true,
        .columnIndex = std::nullopt,
        .message = ""};
}
