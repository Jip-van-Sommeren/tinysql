#include "db_query_validator.h"

#include <cmath>
#include <format>
#include <stdexcept>

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

    std::string resolveColumnName(
        const std::vector<std::string> &parts,
        const std::string &tableName)
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

    HeaderPage schema = catalog.getTableHeader(tableRef.name);

    std::unique_ptr<BoundExpr> where;

    if (statement.where)
    {
        where = bindExpr(*statement.where, schema, tableRef.name);
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

    HeaderPage schema = catalog.getTableHeader(tableRef.name);

    std::unique_ptr<BoundExpr> where;

    if (statement.where)
    {
        where = bindExpr(*statement.where, schema, tableRef.name);
    }

    std::vector<std::uint32_t> projectedColumnIndexes;

    for (const std::unique_ptr<SelectItem> &item : statement.selectList)
    {
        if (dynamic_cast<const WildcardSelectItem *>(item.get()))
        {
            for (const Column &column : schema.columns)
            {
                projectedColumnIndexes.push_back(column.columnIndex);
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

            for (const Column &column : schema.columns)
            {
                projectedColumnIndexes.push_back(column.columnIndex);
            }
            continue;
        }

        if (const auto *exprItem = dynamic_cast<const ExprSelectItem *>(item.get()))
        {
            const auto *columnExpr =
                dynamic_cast<const ColumnExpr *>(exprItem->expr.get());

            if (!columnExpr)
            {
                throw std::runtime_error("Only column expressions are supported in SELECT");
            }

            std::string columnName =
                resolveColumnName(columnExpr->parts, tableRef.name);

            const Column &column = resolveColumn(schema, columnName);
            projectedColumnIndexes.push_back(column.columnIndex);
            continue;
        }

        throw std::runtime_error("Unsupported SELECT item");
    }

    return BoundSelect{
        .tableName = tableRef.name,
        .projectedColumnIndexes = std::move(projectedColumnIndexes),
        .where = std::move(where)};
}

// Constraint QueryValidator(const std::string &name, DataType type, const ColumnDefExpr &colDef)
// {
//     Constraint constraint;
//     for (const auto &constraintExpr : colDef.constraints)
//         {
//             if (constraintExpr->constraintType == ConstraintType::NotNull)
//             {
//                 constraint.type = type;
//                 constraint.constraintType = ConstraintType::NotNull;
//             }
//             else if (constraintExpr->constraintType == ConstraintType::PrimaryKey)
//             {
//                 constraint.type = type;
//                 constraint.constraintType = ConstraintType::PrimaryKey;
//             }
//             // Handle other constraint types as needed
//         }
//     return Constraint{name, type};
// }


BoundCreateTable QueryValidator::validateCreateTable(const CreateTableStatement &statement)
{
    std::vector<Column> columns;
    columns.reserve(statement.columns.size());

    for (const auto &colDef : statement.columns)
    {

        Column column;
        column.name = colDef->columnName;
        column.type = colDef->dataType->type;
        column.nullable = true; // Default to nullable unless specified otherwise
        column.columnIndex = 0; // Will be set later
        column.constraint = Constraint{}; // Default constraint
        column.storage = FixedColumnStorage{.offset = 0, .size = 0}; // Placeholder



        columns.push_back(std::move(column));
    }
    return BoundCreateTable{
        .tableName = statement.tableName,
        .columns = statement.columns};
}

BoundInsert QueryValidator::validateInsert(const InsertStatement &statement)
{
    if (!catalog.tableExists(statement.tableName))
    {
        throw std::runtime_error("Table does not exist: " + statement.tableName);
    }

    HeaderPage schema = catalog.getTableHeader(statement.tableName);

    std::vector<const Column *> targetColumns;
    targetColumns.reserve(
        statement.columns.empty()
            ? schema.columns.size()
            : statement.columns.size());

    if (statement.columns.empty())
    {
        for (const Column &column : schema.columns)
        {
            targetColumns.push_back(&column);
        }
    }
    else
    {
        for (const std::string &columnName : statement.columns)
        {
            targetColumns.push_back(&resolveColumn(schema, columnName));
        }
    }

    if (targetColumns.size() != statement.values.size())
    {
        throw std::runtime_error("INSERT column count does not match value count");
    }

    Row row{
        .values = std::vector<Value>(schema.columns.size(), std::monostate{})};

    for (std::size_t i = 0; i < targetColumns.size(); ++i)
    {
        const Column &column = *targetColumns[i];
        const Expr &expr = *statement.values[i];
        row.values[column.columnIndex] = bindLiteralValue(expr, column);
    }

    for (const Column &column : schema.columns)
    {
        if (!column.nullable &&
            std::holds_alternative<std::monostate>(row.values[column.columnIndex]))
        {
            throw std::runtime_error(
                "Missing value for NOT NULL column: " + column.name);
        }
    }

    return BoundInsert{
        .tableName = statement.tableName,
        .row = std::move(row)};
}

const Column &QueryValidator::resolveColumn(
    const HeaderPage &schema,
    const std::string &columnName) const
{
    for (const Column &column : schema.columns)
    {
        if (column.name == columnName)
        {
            return column;
        }
    }

    throw std::runtime_error(
        std::format("Column does not exist: {}", columnName));
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

    switch (targetColumn.type)
    {
    case DataType::Int:
    {
        const auto *number = dynamic_cast<const NumberExpr *>(&expr);

        if (!number)
        {
            throw std::runtime_error(
                "Expected numeric value for column: " + targetColumn.name);
        }

        if (number->value != std::trunc(number->value))
        {
            throw std::runtime_error(
                "Expected integer value for column: " + targetColumn.name);
        }

        return static_cast<int>(number->value);
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

    default:
        throw std::runtime_error("Unsupported target column type");
    }
}

std::unique_ptr<BoundExpr> QueryValidator::bindExpr(
    const Expr &expr,
    const HeaderPage &schema,
    const std::string &tableName) const
{
    if (const auto *column = dynamic_cast<const ColumnExpr *>(&expr))
    {
        std::string columnName = resolveColumnName(column->parts, tableName);
        const Column &resolved = resolveColumn(schema, columnName);

        return std::make_unique<BoundColumnExpr>(
            resolved.columnIndex,
            resolved.type);
    }

    if (const auto *number = dynamic_cast<const NumberExpr *>(&expr))
    {
        return std::make_unique<BoundLiteralExpr>(
            Value{static_cast<int>(number->value)},
            DataType::Int);
    }

    if (const auto *string = dynamic_cast<const StringExpr *>(&expr))
    {
        return std::make_unique<BoundLiteralExpr>(
            Value{string->value},
            DataType::Text);
    }

    if (dynamic_cast<const NullExpr *>(&expr))
    {
        return std::make_unique<BoundLiteralExpr>(
            Value{std::monostate{}},
            DataType::Null);
    }

    if (const auto *isNull = dynamic_cast<const IsNullExpr *>(&expr))
    {
        return std::make_unique<BoundIsNullExpr>(
            bindExpr(*isNull->operand, schema, tableName),
            isNull->negated);
    }

    if (const auto *cmp = dynamic_cast<const ComparisonExpr *>(&expr))
    {
        auto left = bindExpr(*cmp->left, schema, tableName);
        auto right = bindExpr(*cmp->right, schema, tableName);

        // Optional: validate compatible types here.
        return std::make_unique<BoundComparisonExpr>(
            cmp->op,
            std::move(left),
            std::move(right));
    }

    if (const auto *logical = dynamic_cast<const LogicalExpr *>(&expr))
    {
        return std::make_unique<BoundLogicalExpr>(
            logical->op,
            bindExpr(*logical->left, schema, tableName),
            bindExpr(*logical->right, schema, tableName));
    }

    throw std::runtime_error("Unsupported WHERE expression");
}
