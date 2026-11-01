#pragma once

#include <string>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <unordered_map>

#include "db_storage.h"
#include "db_sql_parser.h"
#include "db_read.h"
#include "db_storage_engine.h"

using TableId = std::uint32_t;

struct ColumnContext
{
    Column column;
    std::uint32_t pos;
};

struct BindContext
{
private:
    std::uint32_t pos_{0};
    std::unordered_map<std::string, std::vector<ColumnContext>> columnsMap{};
    std::unordered_map<std::string, std::vector<BoundConstraint>> constraintsMap{};
    std::unordered_map<std::string, std::string> aliases{};

public:
    std::vector<std::string> tableNames{};
    std::tuple<std::string, std::string> resolveColumnName(
        const std::vector<std::string> &parts) const
    {
        if (parts.size() == 1)
        {
            std::optional<std::string> matchingTable;
            for (const auto &[key, value] : columnsMap)
            {
                for (const auto &colContext : value)
                {
                    if (colContext.column.name == parts[0])
                    {
                        if (matchingTable)
                            throw std::runtime_error("Ambiguous column name: " + parts[0]);
                        matchingTable = key;
                    }
                }
            }
            if (matchingTable)
                return {parts[0], *matchingTable};
            throw std::runtime_error("Unknown column: " + parts[0]);
        }

        if (parts.size() == 2 && columnsMap.contains(parts[0]))
        {
            return {parts[1], parts[0]};
        }

        if (parts.size() == 2 && aliases.contains(parts[0]))
        {
            return {parts[1], aliases.at(parts[0])};
        }

        throw std::runtime_error("Unsupported column reference");
    }

    void insertTableName(const std::string &tableName)
    {
        if (resolveTable(tableName))
            throw std::runtime_error("Duplicate table reference: " + tableName);
        tableNames.push_back(tableName);
    }

    const std::vector<ColumnContext> &getColumns(const std::string &tableName) const
    {
        const auto &name = aliases.contains(tableName) ? aliases.at(tableName) : tableName;
        if (!columnsMap.contains(name))
            throw std::runtime_error("Unknown table: " + tableName);
        return columnsMap.at(name);
    }

    std::vector<ColumnContext> getColumns() const
    {
        std::vector<ColumnContext> columns{};
        for (const auto &tableName : tableNames)
        {
            const auto &val = columnsMap.at(tableName);
            columns.insert(columns.end(), val.begin(), val.end());
        }
        return columns;
    }

    void insertCols(std::vector<Column> cols, const std::string &tableName)
    {

        std::vector<ColumnContext> colContexts{};
        for (auto &col : cols)
        {
            colContexts.push_back(ColumnContext{.column = col, .pos = pos_});
            ++pos_;
        }
        columnsMap.insert({tableName, colContexts});
    }

    void insertConstraints(std::vector<BoundConstraint> constraints, const std::string &tableName)
    {

        constraintsMap.insert({tableName, constraints});
    }

    void insertConstraint(BoundConstraint constraint, const std::string &tableName)
    {
        constraintsMap.at(tableName).push_back(std::move(constraint));
    }

    void insertAlias(const std::string &alias, const std::string &tableName)
    {
        if (aliases.contains(alias) || (columnsMap.contains(alias) && alias != tableName))
            throw std::runtime_error("Duplicate table alias: " + alias);
        aliases.insert({alias, tableName});
    }

    Column &resolveColumn(std::string_view name, const std::string &tableName)
    {
        auto &columns = columnsMap.at(tableName);
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const ColumnContext &c)
            {
                return c.column.name == name;
            });

        if (it == columns.end())
        {
            throw std::runtime_error(
                std::format(
                    "Unknown column '{}.{}'",
                    tableName,
                    name));
        }

        return it->column;
    }

    bool resolveTable(const std::string &targetTableName) const
    {
        if (aliases.contains(targetTableName))
            return true;
        for (const auto &tableName : tableNames)
        {
            if (tableName == targetTableName)
            {
                return true;
            }
        }
        return false;
    }

    const ColumnContext &resolveColumnContext(std::string_view name, const std::string &tableName) const
    {
        const auto &columns = getColumns(tableName);
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const ColumnContext &c)
            {
                return c.column.name == name;
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

    const Column &resolveColumn(std::string_view name, const std::string &tableName) const
    {
        return resolveColumnContext(name, tableName).column;
    }

    const Column &resolveColumn(std::uint32_t columnIndex, const std::string &tableName) const
    {
        if (!columnsMap.contains(tableName))
            throw std::runtime_error("error");
        const auto &columns = columnsMap.at(tableName);
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const ColumnContext &c)
            {
                return c.column.columnIndex == columnIndex;
            });

        if (it == columns.end())
        {
            throw std::runtime_error(
                std::format(
                    "Unknown column '{}.{}'",
                    tableName,
                    columnIndex));
        }

        return it->column;
    }

    const BoundConstraint &resolveConstraint(std::string_view name, const std::string &tableName) const
    {
        if (!constraintsMap.contains(tableName))
            throw std::runtime_error("error");
        const auto &constraints = constraintsMap.at(tableName);
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

    const BoundDefaultConstraintExpr *findDefaultConstraint(ColumnId columnId, const std::string &tableName) const
    {
        if (!constraintsMap.contains(tableName))
            throw std::runtime_error("error");
        const auto &constraints = constraintsMap.at(tableName);
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

    bool hasDefaultConstraint(ColumnId columnId, const std::string &tableName) const
    {
        return findDefaultConstraint(columnId, tableName) != nullptr;
    }

    const BoundDefaultConstraintExpr &getDefaultConstraint(ColumnId columnId, const std::string &tableName) const
    {
        const auto *constraint = findDefaultConstraint(columnId, tableName);
        if (!constraint)
        {
            throw std::runtime_error("No default constraint found for column");
        }

        return *constraint;
    }
};

// struct BindContext
// {
//     std::string tableName;
//     std::vector<Column> columns;
//     std::vector<BoundConstraint> constraints;

//     Column &resolveColumn(std::string_view name)
//     {
//         auto it = std::find_if(
//             columns.begin(),
//             columns.end(),
//             [&](const Column &column)
//             {
//                 return column.name == name;
//             });

//         if (it == columns.end())
//         {
//             throw std::runtime_error(
//                 std::format(
//                     "Unknown column '{}.{}'",
//                     tableName,
//                     name));
//         }

//         return *it;
//     }

//     const Column &resolveColumn(std::string_view name) const
//     {
//         auto it = std::find_if(
//             columns.begin(),
//             columns.end(),
//             [&](const Column &column)
//             {
//                 return column.name == name;
//             });

//         if (it == columns.end())
//         {
//             throw std::runtime_error(
//                 std::format(
//                     "Unknown column '{}.{}'",
//                     tableName,
//                     name));
//         }

//         return *it;
//     }

//     const Column &resolveColumn(std::uint32_t columnIndex) const
//     {
//         auto it = std::find_if(
//             columns.begin(),
//             columns.end(),
//             [&](const Column &column)
//             {
//                 return column.columnIndex == columnIndex;
//             });

//         if (it == columns.end())
//         {
//             throw std::runtime_error(
//                 std::format(
//                     "Unknown column '{}.{}'",
//                     tableName,
//                     columnIndex));
//         }

//         return *it;
//     }

//     const BoundConstraint &resolveConstraint(std::string_view name) const
//     {
//         auto it = std::find_if(
//             constraints.begin(),
//             constraints.end(),
//             [&](const BoundConstraint &constraint)
//             {
//                 return std::visit(
//                     [&](const auto &c)
//                     {
//                         return c.constraintName == name;
//                     },
//                     constraint);
//             });

//         if (it == constraints.end())
//         {
//             throw std::runtime_error(
//                 std::format(
//                     "Unknown constraint '{}.{}'",
//                     tableName,
//                     name));
//         }

//         return *it;
//     }

//     const BoundDefaultConstraintExpr *findDefaultConstraint(ColumnId columnId) const
//     {
//         for (const BoundConstraint &constraint : constraints)
//         {
//             const auto *defaultConstraint =
//                 std::get_if<BoundDefaultConstraintExpr>(&constraint);
//             if (defaultConstraint && defaultConstraint->columnId == columnId)
//             {
//                 return defaultConstraint;
//             }
//         }
//         return nullptr;
//     }

//     bool hasDefaultConstraint(ColumnId columnId) const
//     {
//         return findDefaultConstraint(columnId) != nullptr;
//     }

//     const BoundDefaultConstraintExpr &getDefaultConstraint(ColumnId columnId) const
//     {
//         const auto *constraint = findDefaultConstraint(columnId);
//         if (!constraint)
//         {
//             throw std::runtime_error("No default constraint found for column");
//         }

//         return *constraint;
//     }
// };

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
    BindContext bContext_{};

    BoundSelect validateStatement(const SelectStatement &statement);
    BoundInsert validateStatement(const InsertStatement &statement);
    BoundDelete validateStatement(const DeleteStatement &statement);
    BoundUpdate validateStatement(const UpdateStatement &statement);
    BoundCreateTable validateStatement(const CreateTableStatement &statement);

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
        HeaderPage schema = catalog_.getTableHeader(name);
        BindContext bindContext{};
        bindContext.insertTableName(name);
        bindContext.insertCols(schema.columns, name);
        bindContext.insertConstraints(schema.constraints, name);
        return bindContext;
    }

    void appendContext(const std::string &name)
    {
        HeaderPage schema = catalog_.getTableHeader(name);
        bContext_.insertTableName(name);
        bContext_.insertCols(schema.columns, name);
        bContext_.insertConstraints(schema.constraints, name);
    }

    void appendAlias(const std::string &alias, const std::string &name)
    {
        bContext_.insertAlias(alias, name);
    }

    // return Context{
    //     .tableName = name,
    //     .alias = alias,
    //     .column = std::move(schema.columns),
    //     .constraints = std::move(schema.constraints)};

    DataType
    binaryResultType(
        BinaryOperator op,
        DataType leftType,
        DataType rightType);
};
