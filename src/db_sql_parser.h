#pragma once

#include "db_sql_lexer.h"
#include "db_storage.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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



enum class ArithmeticOp
{
    Add,
    Subtract,
    Multiply,
    Divide
};

enum class BinaryOperator : std::uint8_t
{
    Add,
    Subtract,
    Multiply,
    Divide,
    And,
    Or,
    Eq,
    Ne,
    Gt,
    Ge,
    Lt,
    Le
};

enum class UnaryOperator
{
    Not,
    Negate,
    Positive
};





struct UnaryExpr : Expr
{
    UnaryOperator op;
    std::unique_ptr<Expr> operand;

    UnaryExpr(
        UnaryOperator op,
        std::unique_ptr<Expr> operand);
};

struct BinaryExpr : Expr
{
    BinaryOperator op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    BinaryExpr(
        BinaryOperator op,
        std::unique_ptr<Expr> left,
        std::unique_ptr<Expr> right);
};

struct IsNullExpr : Expr
{
    std::unique_ptr<Expr> operand;
    bool negated; // false = IS NULL, true = IS NOT NULL

    IsNullExpr(std::unique_ptr<Expr> operand, bool negated)
        : operand(std::move(operand)),
          negated(negated)
    {
    }
};

enum class ConstraintType: std::uint8_t
{
    PrimaryKey,
    ForeignKey,
    Unique,
    NotNull,
    Null,
    Default,
    Check
};



struct ConstraintExpr
{
    ConstraintType constraintType;
    std::optional<std::string> constraintName;

    explicit ConstraintExpr(ConstraintType type)
        : constraintType(type)
    {
    }

    virtual ~ConstraintExpr() = default;

    ConstraintType kind() const noexcept
    {
        return kind_;
    }

private:
    ConstraintType kind_;
};


struct PrimaryKeyConstraintExpr : ConstraintExpr
{
    std::vector<std::string> columns;

    explicit PrimaryKeyConstraintExpr(std::vector<std::string> columns)
        : ConstraintExpr(ConstraintType::PrimaryKey),
          columns(std::move(columns))
    {
    }
};

struct UniqueConstraintExpr : ConstraintExpr
{
    std::vector<std::string> columns;

    explicit UniqueConstraintExpr(std::vector<std::string> columns)
        : ConstraintExpr(ConstraintType::Unique),
          columns(std::move(columns))
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
    std::string columnName;

    explicit DefaultConstraintExpr(std::unique_ptr<Expr> value, std::string columnName)
        : ConstraintExpr(ConstraintType::Default),
          value(std::move(value)), columnName(std::move(columnName))
    {
    }
};

struct NotNullConstraintExpr : ConstraintExpr
{
    std::string columnName;

    explicit NotNullConstraintExpr(std::string columnName)
        : ConstraintExpr(ConstraintType::NotNull),
          columnName(std::move(columnName))
    {
    }
};

struct ForeignKeyConstraintExpr : ConstraintExpr
{
    std::vector<std::string> localColumns;
    std::string referencedTable;
    std::vector<std::string> referencedColumns;

    ForeignKeyConstraintExpr(
        std::vector<std::string> localColumns,
        std::string referencedTable,
        std::vector<std::string> referencedColumns)
        : ConstraintExpr(ConstraintType::ForeignKey),
            localColumns(std::move(localColumns)),
          referencedTable(std::move(referencedTable)),
          referencedColumns(std::move(referencedColumns))
    {
    }
};

struct DataTypeExpr : Expr
{
    DataType type;
    std::vector<std::unique_ptr<Expr>> typeArguments;

    explicit DataTypeExpr(
        DataType type,
        std::vector<std::unique_ptr<Expr>> typeArguments = {});
};

struct ColumnDefExpr : Expr
{
    std::string columnName;
    std::unique_ptr<DataTypeExpr> dataType;

    ColumnDefExpr(
        std::string columnName,
        std::unique_ptr<DataTypeExpr> dataType);
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

struct ForeignKeyTableRef : TableRef
{
    std::string tableName;
    std::vector<std::string> columns;
    explicit ForeignKeyTableRef(std::string tableName, std::vector<std::string> columns)
        : tableName(std::move(tableName)),
          columns(std::move(columns))
    {
    }
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
    std::vector<std::unique_ptr<ColumnDefExpr>> columns;      // column name and data type
    std::vector<std::unique_ptr<ConstraintExpr>> constraints; // foreign key references, etc. column level constraint in columndefexpr

    CreateTableStatement(
        std::string tableName,
        std::vector<std::unique_ptr<ColumnDefExpr>> columns,
        std::vector<std::unique_ptr<ConstraintExpr>> constraints);
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

struct ParsedCreateTableElements
{
    std::vector<std::unique_ptr<ColumnDefExpr>> columns;
    std::vector<std::unique_ptr<ConstraintExpr>> constraints;
};

struct ParsedColumnDefinition
{
    std::unique_ptr<ColumnDefExpr> column;
    std::vector<std::unique_ptr<ConstraintExpr>> constraints;
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
    std::unique_ptr<CreateTableStatement> parseCreateTableStatement();

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
    bool isBinaryOperator(const Token &token);
    bool isUnaryOperator(const Token &token);



    BinaryOperator binaryOpFromToken(const Token &token);
    BinaryOperator parseBinaryOp();

    UnaryOperator unaryOpFromToken(const Token &token);
    UnaryOperator parseUnaryOp();


    std::unique_ptr<Expr> parseExpression();
    std::unique_ptr<Expr> parseOr();
    std::unique_ptr<Expr> parseAnd();

    std::unique_ptr<Expr> parseNot();

    std::unique_ptr<Expr> parseAdditive();
    std::unique_ptr<Expr> parseMultiplicative();
    std::unique_ptr<Expr> parseComparison();
    std::unique_ptr<Expr> parseArithmeticUnary();

    std::unique_ptr<Expr> parsePrimary();


    bool isColumnConstraintStart() const;

    bool isTableConstraintStart() const;


    std::optional<std::string> parseOptionalConstraintName();
    std::unique_ptr<ConstraintExpr> parseColumnConstraint(const std::string& columnName);
    std::unique_ptr<ConstraintExpr> parseColumnConstraintBody(const std::string& columnName);


    std::vector<std::string> parseParenthesizedIdentifierList();


    std::unique_ptr<DataTypeExpr> parseDataType();
    ParsedColumnDefinition parseColumnDefinition();
    std::string parseIdentifier();
    std::vector<std::unique_ptr<ConstraintExpr>> parseTableConstraints();

    ParsedCreateTableElements parseCreateTableElements();

    std::unique_ptr<ConstraintExpr> parseTableConstraint();

    std::unique_ptr<ConstraintExpr> parseTableConstraintBody();
    ForeignKeyTableRef parseReferenceClause();

};
