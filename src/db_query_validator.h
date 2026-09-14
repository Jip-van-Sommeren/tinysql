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

using TableId = std::uint32_t;

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
    std::vector<Constraint> constraints;
};

using BoundQuery = std::variant<BoundSelect, BoundInsert, BoundDelete, BoundCreateTable>;

enum class SerializedExprType : std::uint8_t
{
    ColumnReference,
    Literal,
    Binary,
    Unary
};

struct BindContext
{
    std::string tableName;
    std::vector<Column> columns;
    std::vector<Constraint> constraints;

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

    const Constraint &resolveConstraint(std::string_view name) const
    {
        auto it = std::find_if(
            constraints.begin(),
            constraints.end(),
            [&](const Constraint &constraint)
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
        for (const Constraint &constraint : constraints)
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

class Catalog
{
public:
    virtual bool tableExists(const std::string &tableName) const = 0;
    virtual HeaderPage getTableHeader(const std::string &tableName) const = 0;
    virtual BindContext createBindContext(const std::string &tableName) const = 0;
    virtual ~Catalog() = default;
};

class FileCatalog : public Catalog
{
public:
    explicit FileCatalog(std::filesystem::path tablesPath)
        : tablesPath(std::move(tablesPath))
    {
    }

    bool tableExists(const std::string &tableName) const override
    {
        return std::filesystem::exists(tablePath(tableName));
    }

    HeaderPage getTableHeader(const std::string &tableName) const override
    {
        std::filesystem::path path = tablePath(tableName);

        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error("Table does not exist: " + tableName);
        }

        RawPage rawPage = readPageFromFile(path, 0);
        Page page = decodeHeaderPage(rawPage);

        return std::get<HeaderPage>(page.data);
    }

    BindContext createBindContext(const std::string &tableName) const
    {
        HeaderPage schema = getTableHeader(tableName);
        return BindContext{
            .tableName = tableName,
            .columns = std::move(schema.columns),
            .constraints = std::move(schema.constraints)};
    }

private:
    std::filesystem::path tablesPath;

    std::filesystem::path tablePath(const std::string &tableName) const
    {
        return tablesPath / (tableName + ".table");
    }
};

class QueryValidator
{
public:
    explicit QueryValidator(const Catalog &catalog);

    BoundQuery validate(const Statement &statement);

private:
    const Catalog &catalog;

    BoundSelect validateSelect(const SelectStatement &statement);
    BoundInsert validateInsert(const InsertStatement &statement);
    BoundDelete validateDelete(const DeleteStatement &statement);
    BoundCreateTable bindCreateTable(const CreateTableStatement &statement);
    Constraint bindConstraintExpr(const ConstraintExpr &expr, BindContext &context);
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

    DataType binaryResultType(
        BinaryOperator op,
        DataType leftType,
        DataType rightType);
};
