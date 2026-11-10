#pragma once

#include <string>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <format>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include "db_storage.h"
#include "db_sql_parser.h"
#include "db_read.h"
#include "db_catalog.h"

using TableId = std::uint32_t;

struct ColumnContext
{
    Column column;
    std::uint32_t pos;
};

struct TableContext
{
    std::string tableName;
    // The visible qualifier: the explicit alias, or the table name if absent.
    std::string alias;
    std::vector<ColumnContext> columns;
    std::vector<BoundConstraint> constraints;
};

struct BindContext
{
private:
    std::size_t columnCount_{0};
    std::vector<TableContext> tables_;

    std::size_t tableIndex(const std::string &qualifier) const
    {
        for (std::size_t i = 0; i < tables_.size(); ++i)
        {
            if (tables_[i].alias == qualifier)
                return i;
        }
        throw std::runtime_error("Unknown table: " + qualifier);
    }

public:
    void insert(
        const std::string &tableName,
        const std::optional<std::string> &alias,
        const std::vector<Column> &columns,
        const std::vector<BoundConstraint> &constraints = {})
    {
        const std::string qualifier = alias.value_or(tableName);
        if (resolveTable(qualifier))
            throw std::runtime_error("Duplicate table alias: " + qualifier);
        if (columns.size() > std::numeric_limits<std::uint32_t>::max() - columnCount_)
            throw std::runtime_error("Too many columns in binding context");

        TableContext table{
            .tableName = tableName,
            .alias = qualifier,
            .columns = {},
            .constraints = constraints};
        table.columns.reserve(columns.size());
        for (const auto &column : columns)
        {
            // Expression positions address the full FROM row. Schema column
            // indices remain local to their stored table.
            table.columns.push_back({.column = column,
                                     .pos = static_cast<std::uint32_t>(columnCount_ + column.columnIndex)});
        }
        tables_.push_back(std::move(table));
        columnCount_ += columns.size();
    }

    void insert(
        const std::string &tableName,
        const std::optional<std::string> &alias,
        const HeaderPage &schema)
    {
        insert(tableName, alias, schema.columns, schema.constraints);
    }

    bool resolveTable(const std::string &qualifier) const
    {
        return std::any_of(tables_.begin(), tables_.end(), [&](const TableContext &table)
                           { return table.alias == qualifier; });
    }

    std::tuple<std::string, std::string> resolveColumnName(
        const std::vector<std::string> &parts) const
    {
        if (parts.size() == 1)
        {
            const TableContext *matchingTable = nullptr;
            for (const auto &table : tables_)
            {
                for (const auto &column : table.columns)
                {
                    if (column.column.name == parts[0])
                    {
                        if (matchingTable)
                            throw std::runtime_error("Ambiguous column name: " + parts[0]);
                        matchingTable = &table;
                    }
                }
            }
            if (!matchingTable)
                throw std::runtime_error("Unknown column: " + parts[0]);
            return {parts[0], matchingTable->alias};
        }
        if (parts.size() == 2)
        {
            const auto &table = tables_[tableIndex(parts[0])];
            return {parts[1], table.alias};
        }
        throw std::runtime_error("Unsupported column reference");
    }

    const std::vector<ColumnContext> &getColumns(const std::string &qualifier) const
    {
        return tables_[tableIndex(qualifier)].columns;
    }

    std::vector<ColumnContext> wildCardSelect() const
    {
        std::vector<ColumnContext> columns;
        columns.reserve(columnCount_);
        for (const auto &table : tables_)
            columns.insert(columns.end(), table.columns.begin(), table.columns.end());
        return columns;
    }

    const std::vector<ColumnContext> &qualifiedWildCardSelect(const std::string &qualifier) const
    {
        return getColumns(qualifier);
    }

    const ColumnContext &selectItem(const std::vector<std::string> &parts) const
    {
        const auto [name, qualifier] = resolveColumnName(parts);
        return resolveColumnContext(name, qualifier);
    }

    const ColumnContext &resolveColumnContext(const std::string &name, const std::string &qualifier) const
    {
        const auto &columns = getColumns(qualifier);
        const auto column = std::find_if(columns.begin(), columns.end(), [&](const ColumnContext &c)
                                         { return c.column.name == name; });
        if (column == columns.end())
            throw std::runtime_error(std::format("Unknown column '{}.{}'", qualifier, name));
        return *column;
    }

    ColumnContext &resolveColumnContext(const std::string &name, const std::string &qualifier)
    {
        auto &columns = tables_[tableIndex(qualifier)].columns;
        const auto column = std::find_if(columns.begin(), columns.end(), [&](const ColumnContext &c)
                                         { return c.column.name == name; });
        if (column == columns.end())
            throw std::runtime_error(std::format("Unknown column '{}.{}'", qualifier, name));
        return *column;
    }

    const ColumnContext &resolveColumnContext(std::uint32_t columnId, const std::string &qualifier) const
    {
        const auto &columns = getColumns(qualifier);
        const auto column = std::find_if(columns.begin(), columns.end(), [&](const ColumnContext &c)
                                         { return c.column.columnIndex == columnId; });
        if (column == columns.end())
            throw std::runtime_error(std::format("Unknown column '{}.{}'", qualifier, columnId));
        return *column;
    }

    const Column &resolveColumn(const std::string &name, const std::string &qualifier) const
    {
        return resolveColumnContext(name, qualifier).column;
    }

    Column &resolveColumn(const std::string &name, const std::string &qualifier)
    {
        return resolveColumnContext(name, qualifier).column;
    }

    const Column &resolveColumn(std::uint32_t columnId, const std::string &qualifier) const
    {
        return resolveColumnContext(columnId, qualifier).column;
    }

    const std::vector<BoundConstraint> &getConstraints(const std::string &qualifier) const
    {
        return tables_[tableIndex(qualifier)].constraints;
    }

    void insertConstraints(const std::vector<BoundConstraint> &constraints, const std::string &qualifier)
    {
        auto &stored = tables_[tableIndex(qualifier)].constraints;
        stored.insert(stored.end(), constraints.begin(), constraints.end());
    }

    const BoundConstraint &resolveConstraint(const std::string &name, const std::string &qualifier) const
    {
        const auto &constraints = getConstraints(qualifier);
        const auto constraint = std::find_if(constraints.begin(), constraints.end(), [&](const BoundConstraint &c)
                                             { return std::visit([&](const auto &value)
                                                                 { return value.constraintName == name; }, c); });
        if (constraint == constraints.end())
            throw std::runtime_error(std::format("Unknown constraint '{}.{}'", qualifier, name));
        return *constraint;
    }

    const BoundDefaultConstraintExpr *findDefaultConstraint(ColumnId columnId, const std::string &qualifier) const
    {
        for (const auto &constraint : getConstraints(qualifier))
        {
            const auto *defaultConstraint = std::get_if<BoundDefaultConstraintExpr>(&constraint);
            if (defaultConstraint && defaultConstraint->columnId == columnId)
                return defaultConstraint;
        }
        return nullptr;
    }

    bool hasDefaultConstraint(ColumnId columnId, const std::string &qualifier) const
    {
        return findDefaultConstraint(columnId, qualifier) != nullptr;
    }

    const BoundDefaultConstraintExpr &getDefaultConstraint(ColumnId columnId, const std::string &qualifier) const
    {
        const auto *constraint = findDefaultConstraint(columnId, qualifier);
        if (!constraint)
            throw std::runtime_error("No default constraint found for column");
        return *constraint;
    }
};

struct BoundTableRef;
struct BoundNamedTableRef
{
    std::string tableName;
    std::optional<std::string> alias;

    explicit BoundNamedTableRef(std::string tableName, std::optional<std::string> alias = std::nullopt)
        : tableName(std::move(tableName)), alias(std::move(alias))
    {
    }
};

struct BoundForeignKeyReference
{
    std::unique_ptr<BoundTableRef> tableName;
    std::vector<ColumnId> columns;
    explicit BoundForeignKeyReference(std::unique_ptr<BoundTableRef> tableName, std::vector<ColumnId> columns)
        : tableName(std::move(tableName)),
          columns(std::move(columns))
    {
    }
};

struct BoundJoinTableRef
{
    std::unique_ptr<BoundTableRef> left;
    std::unique_ptr<BoundTableRef> right;
    JoinType type;
    std::unique_ptr<BoundExpr> condition;

    BoundJoinTableRef(
        std::unique_ptr<BoundTableRef> left,
        std::unique_ptr<BoundTableRef> right,
        JoinType type,
        std::unique_ptr<BoundExpr> condition) :

                                                left(std::move(left)),
                                                right(std::move(right)),
                                                type(type),
                                                condition(std::move(condition))
    {
    }
};
struct BoundTableRef
{
    std::variant<
        BoundNamedTableRef,
        BoundJoinTableRef>
        value;
};

struct BoundTableResult
{
    BoundTableRef table;
    BindContext context;
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
    // std::string tableName;

    std::vector<BoundSelectItem> projections;

    std::unique_ptr<BoundTableRef> from;

    std::unique_ptr<BoundExpr> where;

    std::vector<std::unique_ptr<BoundExpr>> groupBy;

    std::unique_ptr<BoundExpr> having;
};

struct BoundDelete
{
    std::unique_ptr<BoundTableRef> from;
    std::unique_ptr<BoundExpr> where;
};

struct BoundCreateTable
{
    std::string tableName;
    std::vector<Column> columns;
    std::vector<BoundConstraint> constraints;
};

struct BoundCreateIndex
{
    std::string tableName;
    std::string name;
    std::vector<ColumnId> columnIds;
    bool unique;
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

using BoundQuery = std::variant<BoundSelect, BoundInsert, BoundDelete, BoundCreateTable, BoundCreateIndex, BoundUpdate>;

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
    BindContext bContext_{};

    BoundSelect validateStatement(const SelectStatement &statement);
    BoundInsert validateStatement(const InsertStatement &statement);
    BoundDelete validateStatement(const DeleteStatement &statement);
    BoundUpdate validateStatement(const UpdateStatement &statement);
    BoundCreateTable validateStatement(const CreateTableStatement &statement);
    BoundCreateIndex validateStatement(const CreateIndexStatement &statement);

    BoundTableRef bindTableRef(const TableRef &tableRef);
    BoundConstraint bindConstraintExpr(const ConstraintExpr &expr, const std::string &tableName);
    std::unique_ptr<BoundExpr> bindExpr(
        const Expr &expr,
        bool allowAggregates = false) const;
    Column bindColumnDefinition(
        const ColumnDefExpr &colDef,
        std::uint32_t columnIndex);
    // const Column &resolveColumn(
    //     const HeaderPage &schema,
    //     const std::string &columnName) const;

    Value bindLiteralValue(
        const Expr &expr,
        const Column &targetColumn) const;

    Value convertForColumn(
        const Value &value,
        const Column &targetColumn) const;

    BindContext createBindContext(const std::string &name) const
    {
        BindContext bindContext{};
        bindContext.insert(name, std::nullopt, catalog_.getTableHeader(name));
        return bindContext;
    }

    DataType
    binaryResultType(
        BinaryOperator op,
        DataType leftType,
        DataType rightType);
};
