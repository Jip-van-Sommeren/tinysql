#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

#include "db_storage.h"
#include "db_sql_parser.h"
#include "db_read.h"

struct BoundColumnRef
{
    std::uint32_t columnIndex;
    DataType type;
};

struct BoundLiteral
{
    Value value;
};

struct BoundExpr
{
    virtual ~BoundExpr() = default;
};

struct BoundColumnExpr : BoundExpr
{
    std::uint32_t columnIndex;
    DataType type;

    BoundColumnExpr(std::uint32_t columnIndex, DataType type)
        : columnIndex(columnIndex),
          type(type)
    {
    }
};

struct BoundLiteralExpr : BoundExpr
{
    Value value;
    DataType type;

    BoundLiteralExpr(Value value, DataType type)
        : value(std::move(value)),
          type(type)
    {
    }
};

struct BoundComparisonExpr : BoundExpr
{
    ComparisonOp op;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;

    BoundComparisonExpr(
        ComparisonOp op,
        std::unique_ptr<BoundExpr> left,
        std::unique_ptr<BoundExpr> right)
        : op(op),
          left(std::move(left)),
          right(std::move(right))
    {
    }
};

struct BoundLogicalExpr : BoundExpr
{
    LogicalOp op;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;

    BoundLogicalExpr(
        LogicalOp op,
        std::unique_ptr<BoundExpr> left,
        std::unique_ptr<BoundExpr> right)
        : op(op),
          left(std::move(left)),
          right(std::move(right))
    {
    }
};

struct BoundIsNullExpr : BoundExpr
{
    std::unique_ptr<BoundExpr> operand;
    bool negated;

    BoundIsNullExpr(std::unique_ptr<BoundExpr> operand, bool negated)
        : operand(std::move(operand)),
          negated(negated)
    {
    }
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
};

using BoundQuery = std::variant<BoundSelect, BoundInsert, BoundDelete, BoundCreateTable>;

class Catalog
{
public:
    virtual bool tableExists(const std::string &tableName) const = 0;
    virtual HeaderPage getTableHeader(const std::string &tableName) const = 0;
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
    BoundCreateTable validateCreateTable(const CreateTableStatement &statement);
    std::unique_ptr<BoundExpr> bindExpr(
        const Expr &expr,
        const HeaderPage &schema,
        const std::string &tableName) const;

    const Column &resolveColumn(
        const HeaderPage &schema,
        const std::string &columnName) const;

    Value bindLiteralValue(
        const Expr &expr,
        const Column &targetColumn) const;
};
