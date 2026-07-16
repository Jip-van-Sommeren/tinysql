#pragma once

#include "db_sql_lexer.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct Expr
{
    virtual ~Expr() = default;
};

struct ColumnExpr : Expr
{
    std::vector<std::string> parts;

    explicit ColumnExpr(std::vector<std::string> parts);
};

struct NumberExpr : Expr
{
    double value;

    explicit NumberExpr(double value);
};

struct StringExpr : Expr
{
    std::string value;

    explicit StringExpr(std::string value);
};

struct NullExpr : Expr
{
};

enum class LogicalOp
{
    And,
    Or
};

enum class ComparisonOp
{
    Gt,
    Ge,
    Lt,
    Le,
    Eq,
    Ne
};

enum class DataTypeDef
{
    Int,
    Varchar,
    Date,
    Double,
    Boolean,
    Null
};

enum class ArithmeticOp
{
    Add,
    Subtract,
    Multiply,
    Divide
};

struct LogicalExpr : Expr
{
    LogicalOp op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    LogicalExpr(
        LogicalOp op,
        std::unique_ptr<Expr> left,
        std::unique_ptr<Expr> right);
};

struct ComparisonExpr : Expr
{
    ComparisonOp op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    ComparisonExpr(
        ComparisonOp op,
        std::unique_ptr<Expr> left,
        std::unique_ptr<Expr> right);
};

struct ArithmeticExpr : Expr
{
    ArithmeticOp op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    ArithmeticExpr(
        ArithmeticOp op,
        std::unique_ptr<Expr> left,
        std::unique_ptr<Expr> right);
};

struct ColumnDefExpr : Expr
{
    std::string columnName;
    std::unique_ptr<DataTypeExpr> dataType;
    std::vector<std::unique_ptr<ConstraintExpr>> constraints;

    ColumnDefExpr(
        std::string columnName,
        std::unique_ptr<DataTypeExpr> dataType,
        std::vector<std::unique_ptr<ConstraintExpr>> constraints
    );
};

enum class ConstraintType
{
    PrimaryKey,
    ForeignKey,
    Unique,
    NotNull,
    Null,
    Check
};
struct ConstraintExpr : Expr
{
    ConstraintType constraintType;
    std::optional<std::string> constraintName;

    explicit ConstraintExpr(ConstraintType type)
        : constraintType(type)
    {
    }
};

struct CheckConstraintExpr : ConstraintExpr
{
    std::unique_ptr<Expr> condition;

    explicit CheckConstraintExpr(std::unique_ptr<Expr> condition)
        : ConstraintExpr(ConstraintType::Check),
          condition(std::move(condition))
    {
    }
};

struct DefaultConstraintExpr : ConstraintExpr
{
    std::unique_ptr<Expr> value;

    explicit DefaultConstraintExpr(std::unique_ptr<Expr> value)
        : ConstraintExpr(ConstraintType::Default),
          value(std::move(value))
    {
    }
};

struct ForeignKeyConstraintExpr : ConstraintExpr
{
    std::string referencedTable;
    std::vector<std::string> referencedColumns;

    ForeignKeyConstraintExpr(
        std::string referencedTable,
        std::vector<std::string> referencedColumns
    )
        : ConstraintExpr(ConstraintType::ForeignKey),
          referencedTable(std::move(referencedTable)),
          referencedColumns(std::move(referencedColumns))
    {
    }
};

struct DataTypeExpr : Expr
{
    DataTypeDef type;
    std::vector<std::unique_ptr<Expr>> typeArguments;

    explicit DataTypeExpr(
        DataTypeDef type,
        std::vector<std::unique_ptr<Expr>> typeArguments = {}
    );
};

struct SelectItem
{
    virtual ~SelectItem() = default;
};

struct WildcardSelectItem : SelectItem
{
};

struct QualifiedWildcardSelectItem : SelectItem
{
    std::vector<std::string> qualifierParts;

    explicit QualifiedWildcardSelectItem(std::vector<std::string> qualifierParts);
};

struct ExprSelectItem : SelectItem
{
    std::unique_ptr<Expr> expr;

    explicit ExprSelectItem(std::unique_ptr<Expr> expr);
};

struct TableRef
{
    virtual ~TableRef() = default;
};

struct NamedTableRef : TableRef
{
    std::string name;

    explicit NamedTableRef(std::string name);
};

enum class JoinType
{
    Inner,
    LeftOuter,
    RightOuter,
    FullOuter,
    Cross
};

struct JoinTableRef : TableRef
{
    std::unique_ptr<TableRef> left;
    std::unique_ptr<TableRef> right;
    JoinType type;
    std::unique_ptr<Expr> condition;

    JoinTableRef(
        std::unique_ptr<TableRef> left,
        std::unique_ptr<TableRef> right,
        JoinType type,
        std::unique_ptr<Expr> condition);
};

struct Statement
{
    virtual ~Statement() = default;
};

struct SelectStatement : Statement
{
    std::vector<std::unique_ptr<SelectItem>> selectList;
    std::unique_ptr<TableRef> from;
    std::unique_ptr<Expr> where;

    SelectStatement(
        std::vector<std::unique_ptr<SelectItem>> selectList,
        std::unique_ptr<TableRef> from,
        std::unique_ptr<Expr> where);
};

struct DeleteStatement : Statement
{
    std::unique_ptr<TableRef> from;
    std::unique_ptr<Expr> where;

    DeleteStatement(
        std::unique_ptr<TableRef> from,
        std::unique_ptr<Expr> where);
};

struct CreateTableStatement : Statement
{
    std::string tableName;
    std::vector<std::unique_ptr<ColumnDefExpr>> columns; // column name and data type
    std::vector<std::unique_ptr<ConstraintExpr>> constraints; // foreign key references, etc. column level constraint in columndefexpr

    CreateTableStatement(
        std::string tableName,
        std::vector<std::unique_ptr<Expr>> columns);
};

struct InsertStatement : Statement
{
    std::string tableName;
    std::vector<std::string> columns;
    std::vector<std::unique_ptr<Expr>> values;

    InsertStatement(
        std::string tableName,
        std::vector<std::string> columns,
        std::vector<std::unique_ptr<Expr>> values);
};

struct Assignment
{
    std::vector<std::string> target;
    std::unique_ptr<Expr> value;

    Assignment(
        std::vector<std::string> target,
        std::unique_ptr<Expr> value);
};

struct UpdateStatement : Statement
{
    std::string tableName;
    std::vector<Assignment> assignments;
    std::unique_ptr<Expr> where;

    UpdateStatement(
        std::string tableName,
        std::vector<Assignment> assignments,
        std::unique_ptr<Expr> where);
};

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens);

    std::unique_ptr<Statement> parseStatement();
    std::unique_ptr<SelectStatement> parseSelectStatement();
    std::unique_ptr<DeleteStatement> parseDeleteStatement();
    std::unique_ptr<InsertStatement> parseInsertStatement();
    std::unique_ptr<UpdateStatement> parseUpdateStatement();
    std::unique_ptr<SelectStatement> parseCreateTableStatement();

private:
    std::vector<Token> tokens;
    std::size_t pos;

    const Token &peek() const;
    const Token &previous() const;
    bool isAtEnd() const;
    const Token &advance();
    bool check(TokenType type) const;
    bool checkKeyword(const std::string &keyword) const;
    bool match(TokenType type);
    bool matchKeyword(const std::string &keyword);
    bool checkOperator(const std::string &op) const;
    bool matchOperator(const std::string &op);
    bool matchStar();
    Token expect(TokenType type);
    void expectKeyword(const std::string &keyword);
    void expectOperator(const std::string &op);
    void finishStatement();
    std::unique_ptr<Expr> parseOptionalWhere();
    std::vector<std::string> parseIdentifierParts();
    std::vector<std::string> parseIdentifierList();
    std::vector<std::unique_ptr<Expr>> parseExpressionList();
    Assignment parseAssignment();
    std::unique_ptr<SelectItem> parseSelectItem();
    std::vector<std::unique_ptr<SelectItem>> parseSelectList();
    JoinType parseJoinType();
    std::unique_ptr<TableRef> parseOptionalFrom();
    std::unique_ptr<TableRef> parseRequiredFrom();
    std::unique_ptr<TableRef> parseNamedTableRef();
    std::unique_ptr<TableRef> parseTableRef();
    bool isComparisonOperator(const Token &token);
    bool isArithmeticOperator(const Token &token);
    std::unique_ptr<Expr> parseComparison();
    ComparisonOp comparisonOpFromToken(const Token &token);
    ComparisonOp parseComparisonOp();
    ArithmeticOp arithmeticOpFromToken(const Token &token);
    ArithmeticOp parseArithmeticOp();
    std::unique_ptr<Expr> parsePrimary();
    std::unique_ptr<Expr> parseExpression();
    std::unique_ptr<Expr> parseOr();
    std::unique_ptr<Expr> parseAnd();
};
