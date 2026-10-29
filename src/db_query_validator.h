#pragma once

#include <string>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "db_storage.h"
#include "db_sql_parser.h"
#include "db_read.h"
#include "db_storage_engine.h"

using TableId = std::uint32_t;

struct BindContext
{
    std::string tableName;
    std::vector<Column> columns;
    std::vector<BoundConstraint> constraints;

    Column &resolveColumn(std::string_view name)
    {
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const Column &column)
            {
                return column.name == name;
            });

        if (it == columns.end())
        {
            throw std::runtime_error(
                std::format(
                    "Unknown column '{}.{}'",
                    tableName,
                    name));
        }

        return *it;
    }

    const Column &resolveColumn(std::string_view name) const
    {
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const Column &column)
            {
                return column.name == name;
            });

        if (it == columns.end())
        {
            throw std::runtime_error(
                std::format(
                    "Unknown column '{}.{}'",
                    tableName,
                    name));
        }

        return *it;
    }

    const Column &resolveColumn(std::uint32_t columnIndex) const
    {
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const Column &column)
            {
                return column.columnIndex == columnIndex;
            });

        if (it == columns.end())
        {
            throw std::runtime_error(
                std::format(
                    "Unknown column '{}.{}'",
                    tableName,
                    columnIndex));
        }

        return *it;
    }

    const BoundConstraint &resolveConstraint(std::string_view name) const
    {
        auto it = std::find_if(
            constraints.begin(),
            constraints.end(),
            [&](const BoundConstraint &constraint)
            {
                return std::visit(
                    [&](const auto &c)
                    {
                        return c.constraintName == name;
                    },
                    constraint);
            });

        if (it == constraints.end())
        {
            throw std::runtime_error(
                std::format(
                    "Unknown constraint '{}.{}'",
                    tableName,
                    name));
        }

        return *it;
    }

    const BoundDefaultConstraintExpr *findDefaultConstraint(ColumnId columnId) const
    {
        for (const BoundConstraint &constraint : constraints)
        {
            const auto *defaultConstraint =
                std::get_if<BoundDefaultConstraintExpr>(&constraint);
            if (defaultConstraint && defaultConstraint->columnId == columnId)
            {
                return defaultConstraint;
            }
        }
        return nullptr;
    }

    bool hasDefaultConstraint(ColumnId columnId) const
    {
        return findDefaultConstraint(columnId) != nullptr;
    }

    const BoundDefaultConstraintExpr &getDefaultConstraint(ColumnId columnId) const
    {
        const auto *constraint = findDefaultConstraint(columnId);
        if (!constraint)
        {
            throw std::runtime_error("No default constraint found for column");
        }

        return *constraint;
    }
};

struct BoundNamedTableRef
{
    std::string tableName;

    explicit BoundNamedTableRef(std::string tableName)
        : tableName(std::move(tableName))
    {
    }
};

struct BoundInsert
{
    std::string tableName;
    std::vector<Row> rows; // values ordered by HeaderPage.columns[columnIndex]
};

struct BoundSelectItem
{
    std::unique_ptr<BoundExpr> expr;
    std::string outputName;
};

struct BoundSelect
{
    std::string tableName;

    std::vector<BoundSelectItem> projections;

    std::unique_ptr<BoundExpr> where;

    std::vector<std::unique_ptr<BoundExpr>> groupBy;

    std::unique_ptr<BoundExpr> having;
};

struct BoundDelete
{
    std::string tableName;
    std::unique_ptr<BoundExpr> where;
};

struct BoundCreateTable
{
    std::string tableName;
    std::vector<Column> columns;
    std::vector<BoundConstraint> constraints;
};

struct BoundAssignment
{
    ColumnId columnId;
    std::unique_ptr<BoundExpr> expr;
};

struct BoundUpdate
{
    std::string tableName;
    std::vector<BoundAssignment> assignments;
    std::unique_ptr<BoundExpr> where;
};

using BoundQuery = std::variant<BoundSelect, BoundInsert, BoundDelete, BoundCreateTable, BoundUpdate>;

enum class SerializedExprType : std::uint8_t
{
    ColumnReference,
    Literal,
    Binary,
    Unary
};

class QueryValidator
{
public:
    explicit QueryValidator(const Catalog &Catalog);

    BoundQuery validate(const Statement &statement);

private:
    const Catalog &catalog_;

    BoundSelect validateStatement(const SelectStatement &statement);
    BoundInsert validateStatement(const InsertStatement &statement);
    BoundDelete validateStatement(const DeleteStatement &statement);
    BoundUpdate validateStatement(const UpdateStatement &statement);
    BoundCreateTable validateStatement(const CreateTableStatement &statement);

    BoundConstraint bindConstraintExpr(const ConstraintExpr &expr, BindContext &context);
    std::unique_ptr<BoundExpr> bindExpr(
        const Expr &expr,
        const BindContext &context,
        bool allowAggregates = false) const;
    Column bindColumnDefinition(
        const ColumnDefExpr &colDef,
        std::uint32_t columnIndex);
    // const Column &resolveColumn(
    //     const HeaderPage &schema,
    //     const std::string &columnName) const;

    Value bindLiteralValue(
        const Expr &expr,
        const BindContext &context,
        const Column &targetColumn) const;

    Value convertForColumn(
        const Value &value,
        const Column &targetColumn) const;

    BindContext createBindContext(const std::string &name) const
    {
        HeaderPage schema = catalog_.getTableHeader(name);
        return BindContext{
            .tableName = name,
            .columns = std::move(schema.columns),
            .constraints = std::move(schema.constraints)};
    }

    DataType
    binaryResultType(
        BinaryOperator op,
        DataType leftType,
        DataType rightType);
};
