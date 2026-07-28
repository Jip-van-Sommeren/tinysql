#pragma once

#include <string>
#include <span>
#include <vector>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

#include "db_storage.h"
#include "db_sql_parser.h"
#include "db_read.h"

using TableId = std::uint32_t;
using ColumnId = std::uint32_t;



enum class BoundExprKind : std::uint8_t
{
    Literal,
    ColumnReference,
    Binary,
    Unary,
    IsNull
};

struct BoundLiteral
{
    Value value;
};

struct BoundExpr
{
    explicit BoundExpr(BoundExprKind kind)
        : kind_(kind)
    {
    }

    virtual ~BoundExpr() = default;

    BoundExprKind kind() const noexcept
    {
        return kind_;
    }

private:
    BoundExprKind kind_;
};



struct BoundBinaryExpr final : BoundExpr
{
    BoundBinaryExpr(
        BinaryOperator op,
        std::unique_ptr<BoundExpr> left,
        std::unique_ptr<BoundExpr> right)
        : BoundExpr(BoundExprKind::Binary),
          op(op),
          left(std::move(left)),
          right(std::move(right))
    {
    }

    BinaryOperator op;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;
};

struct BoundUnaryExpr final : BoundExpr
{
    BoundUnaryExpr(
        UnaryOperator op,
        std::unique_ptr<BoundExpr> operand)
        : BoundExpr(BoundExprKind::Unary),
          op(op),
          operand(std::move(operand))
    {
    }

    UnaryOperator op;
    std::unique_ptr<BoundExpr> operand;
};

struct BoundColumnExpr : BoundExpr
{
    std::uint32_t columnIndex;
    DataType type;

    BoundColumnExpr(std::uint32_t columnIndex, DataType type)
        : BoundExpr(BoundExprKind::ColumnReference), columnIndex(columnIndex), type(type)
    {
    }
};

struct BoundLiteralExpr : BoundExpr
{
    Value value;
    DataType type;

    BoundLiteralExpr(Value value, DataType type)
        : BoundExpr(BoundExprKind::Literal),
          value(std::move(value)),
          type(type)
    {
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

struct BoundIsNullExpr : BoundExpr
{
    std::unique_ptr<BoundExpr> operand;
    bool negated;

    BoundIsNullExpr(std::unique_ptr<BoundExpr> operand, bool negated)
        : BoundExpr(BoundExprKind::IsNull),
          operand(std::move(operand)),
          negated(negated)
    {
    }
};




struct BoundConstraintExpr
{
    ConstraintType constraintType;
    std::string constraintName;

    BoundConstraintExpr(
        ConstraintType type,
        std::string constraintName)
        : constraintType(type),
          constraintName(std::move(constraintName))
    {
    }
};


struct BoundPrimaryKeyConstraintExpr : BoundConstraintExpr
{
    std::vector<ColumnId> columnsIds;

    explicit BoundPrimaryKeyConstraintExpr(std::string constraintName, std::vector<ColumnId> columnsIds)
        : BoundConstraintExpr(ConstraintType::PrimaryKey, std::move(constraintName)),
          columnsIds(std::move(columnsIds))
    {
    }
};

struct BoundUniqueConstraintExpr : BoundConstraintExpr
{
    std::vector<ColumnId> columns;

    explicit BoundUniqueConstraintExpr(std::string constraintName, std::vector<ColumnId> columns)
        : BoundConstraintExpr(ConstraintType::Unique, std::move(constraintName)),
          columns(std::move(columns))
    {
    }
};

struct BoundCheckConstraintExpr : BoundConstraintExpr
{
    std::unique_ptr<BoundExpr> condition;

    explicit BoundCheckConstraintExpr(std::string constraintName, std::unique_ptr<BoundExpr> condition)
        : BoundConstraintExpr(ConstraintType::Check, std::move(constraintName)),
          condition(std::move(condition))
    {
    }
};

struct BoundDefaultConstraintExpr : BoundConstraintExpr
{
    std::unique_ptr<BoundExpr> value;
    std::uint32_t columnId;

    explicit BoundDefaultConstraintExpr(std::string constraintName, std::unique_ptr<BoundExpr> value, std::uint32_t columnId)
        : BoundConstraintExpr(ConstraintType::Default, std::move(constraintName)),
          value(std::move(value)),
          columnId(columnId)
    {
    }
};

struct BoundNotNullConstraintExpr : BoundConstraintExpr

{
    explicit BoundNotNullConstraintExpr(std::string constraintName, std::string columnName)
        : BoundConstraintExpr(ConstraintType::NotNull, std::move(constraintName)), 
          columnName(std::move(columnName))
    {
    }

    std::string columnName;

};


struct BoundForeignKeyConstraintExpr final
    : BoundConstraintExpr
{
    BoundForeignKeyConstraintExpr(
        std::string constraintName,
        std::vector<ColumnId> localColumnIds,
        std::string referencedTableName,
        std::vector<ColumnId> referencedColumnIds)
        : BoundConstraintExpr(
              ConstraintType::ForeignKey,
              std::move(constraintName)),
          localColumnIds(std::move(localColumnIds)),
          referencedTableName(std::move(referencedTableName)),
          referencedColumnIds(std::move(referencedColumnIds))
    {
    }

    std::vector<ColumnId> localColumnIds;
    std::string referencedTableName;
    std::vector<ColumnId> referencedColumnIds;
};
struct BoundInsert
{
    std::string tableName;
    Row row; // values ordered by HeaderPage.columns[columnIndex]
};

struct BoundSelect
{
    std::string tableName;
    std::vector<std::uint32_t> projectedColumnIndexes;
    std::unique_ptr<BoundExpr> where;
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
    std::vector<std::unique_ptr<BoundConstraintExpr>> constraints;
};

using BoundQuery = std::variant<BoundSelect, BoundInsert, BoundDelete, BoundCreateTable>;


enum class SerializedExprType : std::uint8_t
{
    ColumnReference,
    Literal,
    Binary,
    Unary
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
            .columns = std::span<Column>(schema.columns)};
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
    BoundCreateTable bindCreateTable(const CreateTableStatement& statement);
    std::unique_ptr<BoundConstraintExpr> bindConstraintExpr(const ConstraintExpr &expr, BindContext &context); 
    std::unique_ptr<BoundExpr> bindExpr(
        const Expr &expr,
        const BindContext &context) const;
    Column bindColumnDefinition(
    const ColumnDefExpr &colDef,
    std::uint32_t columnIndex);
    // const Column &resolveColumn(
    //     const HeaderPage &schema,
    //     const std::string &columnName) const;

    Value bindLiteralValue(
        const Expr &expr,
        const Column &targetColumn) const;
};


struct BindContext
{
    std::string_view tableName;
    std::span<Column> columns;

    Column& resolveColumn(std::string_view name)
    {
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const Column& column)
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

    const Column& resolveColumn(std::string_view name) const
    {
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const Column& column)
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

    const Column& resolveColumn(std::uint32_t columnIndex) const
    {
        auto it = std::find_if(
            columns.begin(),
            columns.end(),
            [&](const Column& column)
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
};