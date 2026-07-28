#include "db_query_validator.h"

#include <cmath>
#include <unordered_set>
#include <format>
#include <map>
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
    BindContext context = catalog.createBindContext(tableRef.name);
    std::unique_ptr<BoundExpr> where;

    if (statement.where)
    {
        where = bindExpr(*statement.where, context);
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

            const Column &column = context.resolveColumn(columnName);
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
    const CreateTableStatement& statement)
{
    BoundCreateTable result;
    result.tableName = statement.tableName;

    // First pass: build all columns.
    for (const auto& columnDef : statement.columns)
    {
        result.columns.push_back(
            bindColumnDefinition(
                *columnDef,
                result.columns.size()));
    }

    BindContext context{
        .tableName = result.tableName,
        .columns = result.columns
    };

    // Second pass: bind constraints.
    for (const auto& constraint : statement.constraints)
    {
        result.constraints.push_back(
            bindConstraintExpr(*constraint, context));
    }

    return result;
}

std::string_view constraintTypeToString(ConstraintType type)
{
    switch (type)
    {
        case ConstraintType::NotNull:
            return "NN";

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
    const std::vector<std::string>& values,
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

std::unique_ptr<BoundConstraintExpr> QueryValidator::bindConstraintExpr(
    const ConstraintExpr &expr, BindContext &context) 

{
    std::string constraintName;


    switch (expr.constraintType)
    {
        case ConstraintType::NotNull:
        {
            const auto& notNullExpr =
                dynamic_cast<const NotNullConstraintExpr&>(expr);
            Column& column = context.resolveColumn(notNullExpr.columnName);
            column.nullable = false; 
            constraintName = expr.constraintName.value_or(
            std::format("{}_{}", constraintTypeToString(notNullExpr.constraintType), notNullExpr.columnName)); // Use a format for the constraint name if not provided

            return std::make_unique<BoundNotNullConstraintExpr>(std::move(constraintName), notNullExpr.columnName);
        }

        case ConstraintType::Default:
        {
            const auto& defaultExpr =
                dynamic_cast<const DefaultConstraintExpr&>(expr);
            
            Column& column = context.resolveColumn(defaultExpr.columnName);
            constraintName = expr.constraintName.value_or(
            std::format("{}_{}", constraintTypeToString(defaultExpr.constraintType), defaultExpr.columnName)); // Use a format for the constraint name if not provided

            auto boundValue = bindExpr(*defaultExpr.value, context);

            return std::make_unique<BoundDefaultConstraintExpr>(
                std::move(constraintName), 
                std::move(boundValue), 
                column.columnIndex);
        }

        case ConstraintType::PrimaryKey:
        {
            const auto& pkExpr =
                dynamic_cast<const PrimaryKeyConstraintExpr&>(expr);

            constraintName = expr.constraintName.value_or(
                std::format(
                    "{}_{}",
                    constraintTypeToString(pkExpr.constraintType),
                    join(pkExpr.columns, "_")));

            std::vector<ColumnId> columnIds;
            columnIds.reserve(pkExpr.columns.size());

        std::unordered_set<ColumnId> seenColumns;

        for (const std::string& columnName : pkExpr.columns)
        {
            Column& column = context.resolveColumn(columnName);

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

            return std::make_unique<BoundPrimaryKeyConstraintExpr>(
                std::move(constraintName),
                std::move(columnIds));
        }

    case ConstraintType::ForeignKey:{
        const auto& fkExpr = dynamic_cast<const ForeignKeyConstraintExpr&>(expr);

        std::vector<ColumnId> localColumnIds;
        localColumnIds.reserve(fkExpr.localColumns.size());

        for (const std::string& name : fkExpr.localColumns)
        {
            const Column& column = context.resolveColumn(name);
            localColumnIds.push_back(column.columnIndex);
        }


        std::vector<ColumnId> referencedColumnIds;
        BindContext referencedTable = catalog.createBindContext(fkExpr.referencedTable);
        referencedColumnIds.reserve(
            fkExpr.referencedColumns.size());

        for (const std::string& name :
            fkExpr.referencedColumns)
        {
            const Column& column =
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
            const Column& local =
                context.resolveColumn(localColumnIds[i]);

            const Column& referenced =
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
                join(fkExpr.referencedColumns, "_")
            )
        );
        return std::make_unique<BoundForeignKeyConstraintExpr>(
            std::move(constraintName),
            std::move(localColumnIds),
            referencedTable.tableName,
            std::move(referencedColumnIds));}

    case ConstraintType::Unique:
        {
            const auto& uniqueExpr =
                dynamic_cast<const UniqueConstraintExpr&>(expr);

            constraintName = expr.constraintName.value_or(
                std::format(
                    "{}_{}",
                    constraintTypeToString(uniqueExpr.constraintType),
                    join(uniqueExpr.columns, "_")));

            std::vector<ColumnId> boundColumns;
            boundColumns.reserve(uniqueExpr.columns.size());

            for (const std::string& columnName : uniqueExpr.columns)
            {
                const Column& column = context.resolveColumn(columnName);
                boundColumns.push_back(column.columnIndex);
            }

            return std::make_unique<BoundUniqueConstraintExpr>(
                std::move(constraintName),
                std::move(boundColumns));
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
    const BindContext &context) const
{
    if (const auto* column =
            dynamic_cast<const ColumnExpr*>(&expr))
    {
        std::string columnName = resolveColumnName(
            column->parts,
            context.tableName);

        const Column& resolved =
            context.resolveColumn(columnName);

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
            bindExpr(*isNull->operand, context),
            isNull->negated);
    }


    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr))
    {
        return std::make_unique<BoundBinaryExpr>(
            binary->op,
            bindExpr(*binary->left, context),
            bindExpr(*binary->right, context));
    }

    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr))
    {
        return std::make_unique<BoundUnaryExpr>(
            unary->op,
            bindExpr(*unary->operand, context));
    }

    throw std::runtime_error("Unsupported expression");
}
