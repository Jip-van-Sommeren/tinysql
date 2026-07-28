#include "db_sql_parser.h"

#include <stdexcept>
#include <utility>

namespace
{
    DataType dataTypeDefFromKeyword(const std::string &keyword)
    {
        if (keyword == "INT")
        {
            return DataType::Int;
        }

        if (keyword == "VARCHAR" || keyword == "TEXT")
        {
            return DataType::Varchar;
        }

        if (keyword == "DATE")
        {
            return DataType::Date;
        }

        if (keyword == "DOUBLE")
        {
            return DataType::Double;
        }

        if (keyword == "BOOLEAN")
        {
            return DataType::Boolean;
        }

        if (keyword == "NULL")
        {
            return DataType::Null;
        }

        throw std::runtime_error("Unknown data type: " + keyword);
    }
}

ColumnExpr::ColumnExpr(std::vector<std::string> parts)
    : parts(std::move(parts)) {}

NumberExpr::NumberExpr(double value)
    : value(value) {}

StringExpr::StringExpr(std::string value)
    : value(std::move(value)) {}


BinaryExpr::BinaryExpr(
    BinaryOperator op,
    std::unique_ptr<Expr> left,
    std::unique_ptr<Expr> right)
    : op(op),
      left(std::move(left)),
      right(std::move(right)) {}


UnaryExpr::UnaryExpr(
    UnaryOperator op,
    std::unique_ptr<Expr> operand)
    : op(op),
      operand(std::move(operand)) {}



DataTypeExpr::DataTypeExpr(
    DataType type,
    std::vector<std::unique_ptr<Expr>> typeArguments)
    : type(type),
      typeArguments(std::move(typeArguments)) {}

ColumnDefExpr::ColumnDefExpr(
    std::string columnName,
    std::unique_ptr<DataTypeExpr> dataType)
    : columnName(std::move(columnName)),
      dataType(std::move(dataType)) {}

QualifiedWildcardSelectItem::QualifiedWildcardSelectItem(std::vector<std::string> qualifierParts)
    : qualifierParts(std::move(qualifierParts)) {}

ExprSelectItem::ExprSelectItem(std::unique_ptr<Expr> expr)
    : expr(std::move(expr)) {}

NamedTableRef::NamedTableRef(std::string name)
    : name(std::move(name)) {}

JoinTableRef::JoinTableRef(
    std::unique_ptr<TableRef> left,
    std::unique_ptr<TableRef> right,
    JoinType type,
    std::unique_ptr<Expr> condition)
    : left(std::move(left)),
      right(std::move(right)),
      type(type),
      condition(std::move(condition)) {}

SelectStatement::SelectStatement(
    std::vector<std::unique_ptr<SelectItem>> selectList,
    std::unique_ptr<TableRef> from,
    std::unique_ptr<Expr> where)
    : selectList(std::move(selectList)),
      from(std::move(from)),
      where(std::move(where)) {}

DeleteStatement::DeleteStatement(
    std::unique_ptr<TableRef> from,
    std::unique_ptr<Expr> where)
    : from(std::move(from)),
      where(std::move(where)) {}

// CreateTableStatement::CreateTableStatement(
//     std::string tableName,
//     std::vector<std::unique_ptr<ColumnDefExpr>> columns)
//     : tableName(std::move(tableName)),
//       columns(std::move(columns)),
//       constraints() {}

CreateTableStatement::CreateTableStatement(

    std::string tableName,
    std::vector<std::unique_ptr<ColumnDefExpr>> columns,      // column name and data type
    std::vector<std::unique_ptr<ConstraintExpr>> constraints) // foreign key references, etc. column level constraint in columndefexpr

    : tableName(std::move(tableName)),
      columns(std::move(columns)),
      constraints(std::move(constraints))
    {}

InsertStatement::InsertStatement(
    std::string tableName,
    std::vector<std::string> columns,
    std::vector<std::unique_ptr<Expr>> values)
    : tableName(std::move(tableName)),
      columns(std::move(columns)),
      values(std::move(values)) {}

Assignment::Assignment(
    std::vector<std::string> target,
    std::unique_ptr<Expr> value)
    : target(std::move(target)),
      value(std::move(value)) {}

UpdateStatement::UpdateStatement(
    std::string tableName,
    std::vector<Assignment> assignments,
    std::unique_ptr<Expr> where)
    : tableName(std::move(tableName)),
      assignments(std::move(assignments)),
      where(std::move(where)) {}

Parser::Parser(std::vector<Token> tokens)
    : tokens(std::move(tokens)), pos(0) {}

std::unique_ptr<Statement> Parser::parseStatement()
{
    if (checkKeyword("SELECT"))
    {
        return parseSelectStatement();
    }
    if (checkKeyword("DELETE"))
    {
        return parseDeleteStatement();
    }
    if (checkKeyword("INSERT"))
    {
        return parseInsertStatement();
    }
    if (checkKeyword("UPDATE"))
    {
        return parseUpdateStatement();
    }
    if (checkKeyword("CREATE"))
    {
        return parseCreateTableStatement();
    }

    throw std::runtime_error("Expected SQL statement, got: " + peek().text);
}


std::unique_ptr<CreateTableStatement> Parser::parseCreateTableStatement()
{
    expectKeyword("CREATE");
    expectKeyword("TABLE");

    auto tableNameToken = expect(TokenType::Identifier);
    std::string tableName = std::move(tableNameToken.text);

    auto elements = parseCreateTableElements();

    finishStatement();

    return std::make_unique<CreateTableStatement>(
        std::move(tableName),
        std::move(elements.columns),
        std::move(elements.constraints));
}

std::vector<std::unique_ptr<ConstraintExpr>> Parser::parseTableConstraints()
{
    std::vector<std::unique_ptr<ConstraintExpr>> constraints;

    while (isTableConstraintStart())
    {
        constraints.push_back(parseTableConstraint());
    }

    return constraints;
}

ParsedCreateTableElements Parser::parseCreateTableElements()
{
    ParsedCreateTableElements result;

    expect(TokenType::LeftParen);

    while (!check(TokenType::RightParen))
    {
        if (isTableConstraintStart())
        {
            result.constraints.push_back(parseTableConstraint());
        }
        else
        {
            auto parsedColumn = parseColumnDefinition();

            result.columns.push_back(std::move(parsedColumn.column));

            for (auto& constraint : parsedColumn.constraints)
            {

                result.constraints.push_back(std::move(constraint));
            }
        }

        if (!match(TokenType::Comma))
        {
            break;
        }
    }

    expect(TokenType::RightParen);

    return result;
}

// std::vector<std::unique_ptr<ColumnDefExpr>> Parser::parseColumnDefinitions()
// {
//     std::vector<std::unique_ptr<ColumnDefExpr>> items;

//     items.push_back(parseColumnDefinition());

//     while (match(TokenType::Comma))
//     {
//         items.push_back(parseColumnDefinition());
//     }

//     return items;
// }

// std::unique_ptr<ColumnDefExpr> Parser::parseColumnDefinition()
// {
//     Token columnNameToken = expect(TokenType::Identifier);
//     std::string columnName = std::move(columnNameToken.text);

//     auto dataType = parseDataType();

//     std::vector<std::unique_ptr<ConstraintExpr>> constraints;
//     if (isColumnConstraintStart())
//     {
//         constraints = parseColumnConstraints();
//     }

//     return std::make_unique<ColumnDefExpr>(
//         std::move(columnName),
//         std::move(dataType),
//         std::move(constraints));
// }

ParsedColumnDefinition Parser::parseColumnDefinition()
{
    auto columnNameToken = expect(TokenType::Identifier);
    std::string columnName = std::move(columnNameToken.text);

    auto dataType = parseDataType();

    std::vector<std::unique_ptr<ConstraintExpr>> constraints;

    while (isColumnConstraintStart())
    {
        constraints.push_back(parseColumnConstraint(columnName));
    }

    auto column = std::make_unique<ColumnDefExpr>(
        std::move(columnName),
        std::move(dataType));

    return {
        std::move(column),
        std::move(constraints)
    };
}

std::unique_ptr<DataTypeExpr> Parser::parseDataType()
{
    Token dataTypeToken = expect(TokenType::Keyword);
    DataType type = dataTypeDefFromKeyword(dataTypeToken.text);

    std::vector<std::unique_ptr<Expr>> typeArguments;
    if (match(TokenType::LeftParen))
    {
        typeArguments.push_back(parseExpression());

        while (match(TokenType::Comma))
        {
            typeArguments.push_back(parseExpression());
        }

        expect(TokenType::RightParen);
    }

    return std::make_unique<DataTypeExpr>(
        type,
        std::move(typeArguments));
}

// std::vector<std::unique_ptr<ConstraintExpr>> Parser::parseColumnConstraints()
// {
//     std::vector<std::unique_ptr<ConstraintExpr>> constraints;

//     while (
//         isColumnConstraintStart())
//     {
//         constraints.push_back(parseConstraint());
//     }

//     return constraints;
// }

bool Parser::isColumnConstraintStart() const
{
    return checkKeyword("CONSTRAINT") ||
           checkKeyword("DEFAULT") ||
           checkKeyword("PRIMARY") ||
           checkKeyword("NOT") ||
           checkKeyword("UNIQUE") ||
           checkKeyword("NULL") ||
           checkKeyword("CHECK") ||
           checkKeyword("REFERENCES");
}

bool Parser::isTableConstraintStart() const
{
    return checkKeyword("CONSTRAINT") ||
           checkKeyword("PRIMARY") ||
           checkKeyword("FOREIGN") ||
           checkKeyword("UNIQUE") ||
           checkKeyword("CHECK");
}
std::optional<std::string> Parser::parseOptionalConstraintName()
{
    if (matchKeyword("CONSTRAINT"))
    {
        return parseIdentifier();
    }

    return std::nullopt;
}

std::unique_ptr<ConstraintExpr> Parser::parseColumnConstraint(const std::string& columnName)
{
    auto name = parseOptionalConstraintName();
    auto constraint = parseColumnConstraintBody(columnName);

    constraint->constraintName = std::move(name);
    return constraint;
}

std::vector<std::string> Parser::parseParenthesizedIdentifierList()
{
    expect(TokenType::LeftParen);
    std::vector<std::string> identifiers;
    identifiers.push_back(parseIdentifier());

    while (match(TokenType::Comma))
    {
        identifiers.push_back(parseIdentifier());
    }

    expect(TokenType::RightParen);
    return identifiers;
}

std::unique_ptr<ConstraintExpr> Parser::parseColumnConstraintBody(const std::string& columnName)
{
    if (matchKeyword("DEFAULT"))
    {
        auto constraint = std::make_unique<DefaultConstraintExpr>(
            parseExpression());

        constraint->value = std::move(constraint->value);
        return constraint;
    }

    if (matchKeyword("PRIMARY"))
    {
        expectKeyword("KEY");

        auto constraint = std::make_unique<PrimaryKeyConstraintExpr>(
            ConstraintType::PrimaryKey);

        constraint->columns.push_back(columnName);
        return constraint;
    }

    if (matchKeyword("NOT"))
    {
        expectKeyword("NULL");

        auto constraint = std::make_unique<ConstraintExpr>(
            ConstraintType::NotNull);

        // constraint->name = columnName;
        return constraint;
    }

    if (matchKeyword("UNIQUE"))
    {
        auto constraint = std::make_unique<UniqueConstraintExpr>(
            ConstraintType::Unique);

        constraint->columns.push_back(columnName);
        return constraint;
    }

    if (matchKeyword("NULL"))
    {
        auto constraint = std::make_unique<ConstraintExpr>(
            ConstraintType::Null);

        // constraint->columns.push_back(columnName);
        return constraint;
    }

    if (matchKeyword("CHECK"))
    {
        expect(TokenType::LeftParen);

        auto condition = parseExpression();

        expect(TokenType::RightParen);

        auto constraint = std::make_unique<CheckConstraintExpr>(
            std::move(condition));
        // constraint->columns.push_back(columnName);
        return constraint;
    }

    if (matchKeyword("REFERENCES"))
    {
        auto reference = parseReferenceClause();

        return std::make_unique<ForeignKeyConstraintExpr>(
            NamedTableRef(std::move(reference.tableName)),
            std::move(reference.columns));
    }

    throw std::runtime_error(
        "Expected column constraint, found: " + peek().text);
}

ForeignKeyTableRef Parser::parseReferenceClause()
{


    Token tableToken = expect(TokenType::Identifier);
    std::string tableName = std::move(tableToken.text);

    std::vector<std::string> columns;
    if (match(TokenType::LeftParen))
    {
        columns = parseIdentifierList();
        expect(TokenType::RightParen);
    }

    return ForeignKeyTableRef{std::move(tableName), std::move(columns)};
}

std::unique_ptr<ConstraintExpr> Parser::parseTableConstraint()
{
    auto name = parseOptionalConstraintName();
    auto constraint = parseTableConstraintBody();

    constraint->constraintName = std::move(name);
    return constraint;
}

// std::unique_ptr<ConstraintExpr> Parser::parseConstraint()
// {
//     std::optional<std::string> name;

//     if (matchKeyword("CONSTRAINT"))
//     {
//         name = parseIdentifier();
//     }

//     auto constraint = parseConstraintBody();
//     constraint->constraintName = std::move(name);

//     return constraint;
// }

std::unique_ptr<ConstraintExpr> Parser::parseTableConstraintBody()
{
    if (matchKeyword("PRIMARY"))
    {
        expectKeyword("KEY");

        auto columns = parseParenthesizedIdentifierList();

        auto constraint = std::make_unique<PrimaryKeyConstraintExpr>(
            ConstraintType::PrimaryKey);

        constraint->columns = std::move(columns);
        return constraint;
    }

    if (matchKeyword("UNIQUE"))
    {
        auto columns = parseParenthesizedIdentifierList();

        auto constraint = std::make_unique<UniqueConstraintExpr>(
            ConstraintType::Unique);

        constraint->columns = std::move(columns);
        return constraint;
    }

    if (matchKeyword("CHECK"))
    {
        expect(TokenType::LeftParen);

        auto condition = parseExpression();

        expect(TokenType::RightParen);

        return std::make_unique<CheckConstraintExpr>(
            std::move(condition));
    }

    if (matchKeyword("FOREIGN"))
    {
        expectKeyword("KEY");

        auto localColumns = parseParenthesizedIdentifierList();
        expectKeyword("REFERENCES");
        auto reference = parseReferenceClause();

        return std::make_unique<ForeignKeyConstraintExpr>(
            std::move(localColumns),
            std::move(reference.tableName),
            std::move(reference.columns));
    }

    throw std::runtime_error(
        "Expected table constraint, found: " + peek().text);
}

// std::unique_ptr<ConstraintExpr> Parser::parseConstraintBody()
// {
//     if (matchKeyword("DEFAULT"))
//     {
//         return std::make_unique<DefaultConstraintExpr>(
//             parseExpression());
//     }

//     if (matchKeyword("PRIMARY"))
//     {
//         expectKeyword("KEY");

//         return std::make_unique<ConstraintExpr>(
//             ConstraintType::PrimaryKey);
//     }

//     if (matchKeyword("NOT"))
//     {
//         expectKeyword("NULL");

//         return std::make_unique<ConstraintExpr>(
//             ConstraintType::NotNull);
//     }

//     if (matchKeyword("UNIQUE"))
//     {
//         return std::make_unique<ConstraintExpr>(
//             ConstraintType::Unique);
//     }

//     if (matchKeyword("NULL"))
//     {
//         return std::make_unique<ConstraintExpr>(
//             ConstraintType::Null);
//     }

//     if (matchKeyword("CHECK"))
//     {
//         expect(TokenType::LeftParen);

//         auto condition = parseExpression();

//         expect(TokenType::RightParen);

//         return std::make_unique<CheckConstraintExpr>(
//             std::move(condition));
//     }

//     throw std::runtime_error(
//         "Expected column constraint, found: " + peek().text);
// }

std::unique_ptr<SelectStatement> Parser::parseSelectStatement()
{
    expectKeyword("SELECT");

    auto selectList = parseSelectList();
    auto from = parseOptionalFrom();
    auto where = parseOptionalWhere();
    finishStatement();

    return std::make_unique<SelectStatement>(
        std::move(selectList),
        std::move(from),
        std::move(where));
}

std::unique_ptr<DeleteStatement> Parser::parseDeleteStatement()
{
    expectKeyword("DELETE");

    auto from = parseRequiredFrom();
    auto where = parseOptionalWhere();
    finishStatement();

    return std::make_unique<DeleteStatement>(
        std::move(from),
        std::move(where));
}

std::unique_ptr<InsertStatement> Parser::parseInsertStatement()
{
    expectKeyword("INSERT");
    expectKeyword("INTO");

    Token table = expect(TokenType::Identifier);
    std::string tableName = std::move(table.text);
    std::vector<std::string> columns;

    if (match(TokenType::LeftParen))
    {
        columns = parseIdentifierList();
        expect(TokenType::RightParen);
    }

    expectKeyword("VALUES");
    expect(TokenType::LeftParen);
    auto values = parseExpressionList();
    expect(TokenType::RightParen);
    finishStatement();

    return std::make_unique<InsertStatement>(
        std::move(tableName),
        std::move(columns),
        std::move(values));
}

std::unique_ptr<UpdateStatement> Parser::parseUpdateStatement()
{
    expectKeyword("UPDATE");

    Token table = expect(TokenType::Identifier);
    std::string tableName = std::move(table.text);

    expectKeyword("SET");
    std::vector<Assignment> assignments;
    assignments.push_back(parseAssignment());
    while (match(TokenType::Comma))
    {
        assignments.push_back(parseAssignment());
    }

    auto where = parseOptionalWhere();
    finishStatement();

    return std::make_unique<UpdateStatement>(
        std::move(tableName),
        std::move(assignments),
        std::move(where));
}

const Token &Parser::peek() const
{
    return tokens[pos];
}

const Token &Parser::previous() const
{
    return tokens[pos - 1];
}

bool Parser::isAtEnd() const
{
    return peek().type == TokenType::End;
}

const Token &Parser::advance()
{
    if (!isAtEnd())
    {
        pos++;
    }

    return previous();
}

bool Parser::check(TokenType type) const
{
    return peek().type == type;
}

bool Parser::checkKeyword(const std::string &keyword) const
{
    return peek().type == TokenType::Keyword && peek().text == keyword;
}

bool Parser::match(TokenType type)
{
    if (check(type))
    {
        advance();
        return true;
    }

    return false;
}

bool Parser::matchKeyword(const std::string &keyword)
{
    if (checkKeyword(keyword))
    {
        advance();
        return true;
    }

    return false;
}

bool Parser::checkOperator(const std::string &op) const
{
    return peek().type == TokenType::Operator && peek().text == op;
}

bool Parser::matchOperator(const std::string &op)
{
    if (checkOperator(op))
    {
        advance();
        return true;
    }

    return false;
}

bool Parser::matchStar()
{
    if (match(TokenType::Star))
    {
        return true;
    }

    return matchOperator("*");
}

Token Parser::expect(TokenType type)
{
    if (!check(type))
    {
        throw std::runtime_error("Unexpected token: " + peek().text);
    }

    return advance();
}

void Parser::expectKeyword(const std::string &keyword)
{
    if (!matchKeyword(keyword))
    {
        throw std::runtime_error("Expected keyword: " + keyword);
    }
}

void Parser::expectOperator(const std::string &op)
{
    if (!matchOperator(op))
    {
        throw std::runtime_error("Expected operator: " + op);
    }
}

void Parser::finishStatement()
{
    match(TokenType::Semicolon);
    expect(TokenType::End);
}

std::unique_ptr<Expr> Parser::parseOptionalWhere()
{
    if (matchKeyword("WHERE"))
    {
        return parseExpression();
    }

    return nullptr;
}

std::string Parser::parseIdentifier()
{

    Token name = expect(TokenType::Identifier);
    return std::string{std::move(name.text)};
}

std::vector<std::string> Parser::parseIdentifierParts()
{
    std::vector<std::string> parts;

    Token name = expect(TokenType::Identifier);
    parts.push_back(std::move(name.text));

    while (match(TokenType::Dot))
    {
        name = expect(TokenType::Identifier);
        parts.push_back(std::move(name.text));
    }

    return parts;
}

std::vector<std::string> Parser::parseIdentifierList()
{
    std::vector<std::string> identifiers;

    Token name = expect(TokenType::Identifier);
    identifiers.push_back(std::move(name.text));

    while (match(TokenType::Comma))
    {
        name = expect(TokenType::Identifier);
        identifiers.push_back(std::move(name.text));
    }

    return identifiers;
}

std::vector<std::unique_ptr<Expr>> Parser::parseExpressionList()
{
    std::vector<std::unique_ptr<Expr>> expressions;

    expressions.push_back(parseExpression());

    while (match(TokenType::Comma))
    {
        expressions.push_back(parseExpression());
    }

    return expressions;
}

Assignment Parser::parseAssignment()
{
    auto target = parseIdentifierParts();
    expectOperator("=");
    auto value = parseExpression();

    return Assignment(std::move(target), std::move(value));
}

std::unique_ptr<SelectItem> Parser::parseSelectItem()
{
    if (matchStar())
    {
        return std::make_unique<WildcardSelectItem>();
    }

    if (check(TokenType::Identifier))
    {
        std::size_t savedPos = pos;

        std::vector<std::string> parts;
        Token name = expect(TokenType::Identifier);
        parts.push_back(std::move(name.text));

        while (match(TokenType::Dot))
        {
            if (matchStar())
            {
                return std::make_unique<QualifiedWildcardSelectItem>(
                    std::move(parts));
            }

            name = expect(TokenType::Identifier);
            parts.push_back(std::move(name.text));
        }

        pos = savedPos;
    }

    auto expr = parseExpression();

    return std::make_unique<ExprSelectItem>(
        std::move(expr));
}

std::vector<std::unique_ptr<SelectItem>> Parser::parseSelectList()
{
    std::vector<std::unique_ptr<SelectItem>> items;

    items.push_back(parseSelectItem());

    while (match(TokenType::Comma))
    {
        items.push_back(parseSelectItem());
    }

    return items;
}

JoinType Parser::parseJoinType()
{
    if (matchKeyword("JOIN"))
    {
        return JoinType::Inner;
    }

    if (matchKeyword("INNER"))
    {
        expectKeyword("JOIN");
        return JoinType::Inner;
    }

    if (matchKeyword("LEFT"))
    {
        matchKeyword("OUTER");
        expectKeyword("JOIN");
        return JoinType::LeftOuter;
    }

    if (matchKeyword("RIGHT"))
    {
        matchKeyword("OUTER");
        expectKeyword("JOIN");
        return JoinType::RightOuter;
    }

    if (matchKeyword("FULL"))
    {
        matchKeyword("OUTER");
        expectKeyword("JOIN");
        return JoinType::FullOuter;
    }

    if (matchKeyword("CROSS"))
    {
        expectKeyword("JOIN");
        return JoinType::Cross;
    }

    throw std::runtime_error("Expected join type");
}

std::unique_ptr<TableRef> Parser::parseOptionalFrom()
{
    if (!matchKeyword("FROM"))
    {
        return nullptr;
    }

    return parseTableRef();
}

std::unique_ptr<TableRef> Parser::parseRequiredFrom()
{
    expectKeyword("FROM");
    return parseTableRef();
}

std::unique_ptr<TableRef> Parser::parseNamedTableRef()
{
    Token name = expect(TokenType::Identifier);
    return std::make_unique<NamedTableRef>(std::move(name.text));
}

std::unique_ptr<TableRef> Parser::parseTableRef()
{
    auto left = parseNamedTableRef();

    while (checkKeyword("JOIN") ||
           checkKeyword("INNER") ||
           checkKeyword("LEFT") ||
           checkKeyword("RIGHT") ||
           checkKeyword("FULL") ||
           checkKeyword("CROSS"))
    {
        JoinType type = parseJoinType();
        auto right = parseNamedTableRef();

        std::unique_ptr<Expr> condition;
        if (type != JoinType::Cross)
        {
            expectKeyword("ON");
            condition = parseExpression();
        }

        left = std::make_unique<JoinTableRef>(
            std::move(left),
            std::move(right),
            type,
            std::move(condition));
    }

    return left;
}

bool Parser::isComparisonOperator(const Token &token)
{
    if (token.type != TokenType::Operator)
    {
        return false;
    }

    return token.text == "=" ||
           token.text == "<>" ||
           token.text == "<" ||
           token.text == "<=" ||
           token.text == ">" ||
           token.text == ">=";
}

bool Parser::isArithmeticOperator(const Token &token)
{
    if (token.type != TokenType::Operator)
    {
        return false;
    }

    return token.text == "+" ||
           token.text == "-" ||
           token.text == "*" ||
           token.text == "/";
}

bool Parser::isBinaryOperator(const Token &token)
{
    if (token.type != TokenType::Operator)
    {
        return false;
    }

    return token.text == "AND" ||
           token.text == "OR" ||
           isComparisonOperator(token) ||
           isArithmeticOperator(token);
}

bool Parser::isUnaryOperator(const Token &token)
{
    if (token.type != TokenType::Operator)
    {
        return false;
    }

    return token.text == "NOT" ||
           token.text == "-" ||
           token.text == "+";
}




// parseExpression()
    // -> parseLogicalOr()
    // -> parseLogicalAnd()
    // -> parseComparison()
    // -> parseAdditive()
    // -> parseMultiplicative()
    // -> parseUnary()
    // -> parsePrimary()


BinaryOperator Parser::binaryOpFromToken(const Token &token)
{
    if (token.type != TokenType::Operator)
    {
        throw std::runtime_error("Incorrect token type was passed");
    }
    if (token.text == "AND")
    {
        return BinaryOperator::And;
    }
    if (token.text == "OR")
    {
        return BinaryOperator::Or;
    }
        if (token.text == "+")
    {
        return BinaryOperator::Add;
    }
    if (token.text == "-")
    {
        return BinaryOperator::Subtract;
    }
    if (token.text == "*")
    {
        return BinaryOperator::Multiply;
    }
    if (token.text == "/")
    {
        return BinaryOperator::Divide;
    }

    if (token.text == "=")
    {
        return BinaryOperator::Eq;
    }
    if (token.text == ">=")
    {
        return BinaryOperator::Ge;
    }
    if (token.text == ">")
    {
        return BinaryOperator::Gt;
    }
    if (token.text == "<=")
    {
        return BinaryOperator::Le;
    }
    if (token.text == "<")
    {
        return BinaryOperator::Lt;
    }
    if (token.text == "<>")
    {
        return BinaryOperator::Ne;
    }
    throw std::runtime_error("Unknown binary operator token");
}

BinaryOperator Parser::parseBinaryOp()
{
    Token token = expect(TokenType::Operator);
    return binaryOpFromToken(token);
}

UnaryOperator Parser::unaryOpFromToken(const Token &token)
{
    if (token.type != TokenType::Operator)
    {
        throw std::runtime_error("Incorrect token type was passed");
    }
    if (token.text == "-")
    {
        return UnaryOperator::Negate;
    }
        if (token.text == "+")
    {
        return UnaryOperator::Positive;
    }
    if (token.text == "NOT")
    {
        return UnaryOperator::Not;
    }
    throw std::runtime_error("Unknown unary operator token");
}

UnaryOperator Parser::parseUnaryOp()
{
    Token token = expect(TokenType::Operator);
    return unaryOpFromToken(token);
}

std::unique_ptr<Expr> Parser::parsePrimary()
{
    if (check(TokenType::Identifier))
    {
        return std::make_unique<ColumnExpr>(parseIdentifierParts());
    }

    if (check(TokenType::Number))
    {
        double value = std::stod(advance().text);
        return std::make_unique<NumberExpr>(value);
    }

    if (check(TokenType::String))
    {
        std::string value = advance().text;
        return std::make_unique<StringExpr>(std::move(value));
    }

    if (matchKeyword("NULL"))
    {
        return std::make_unique<NullExpr>();
    }

    throw std::runtime_error("Expected expression, got: " + peek().text);
}

std::unique_ptr<Expr> Parser::parseExpression()
{
    return parseOr();
}

std::unique_ptr<Expr> Parser::parseOr()
{
    auto left = parseAnd();

    while (matchKeyword("OR"))
    {
        auto right = parseAnd();
        left = std::make_unique<BinaryExpr>(
            BinaryOperator::Or,
            std::move(left),
            std::move(right));
    }

    return left;
}

std::unique_ptr<Expr> Parser::parseAnd()
{
    auto left = parseComparison();

    while (matchKeyword("AND"))
    {
        auto right = parseComparison();
        left = std::make_unique<BinaryExpr>(
            BinaryOperator::And,
            std::move(left),
            std::move(right));
    }

    return left;
}




std::unique_ptr<Expr> Parser::parseNot(){
    if (matchKeyword("NOT"))
    {
        auto operand = parseNot();
        return std::make_unique<UnaryExpr>(
            UnaryOperator::Not,
            std::move(operand));
    }

    return parseComparison();
}

std::unique_ptr<Expr> Parser::parseComparison()
{
    auto left = parseAdditive();


    while (isComparisonOperator(peek()))
    {
        BinaryOperator op = parseBinaryOp();
        auto right = parseAdditive();

        left = std::make_unique<BinaryExpr>(
            op,
            std::move(left),
            std::move(right));
    }


    return left;
}


std::unique_ptr<Expr> Parser::parseAdditive()
{
    auto left = parseMultiplicative();

    while (isArithmeticOperator(peek()) && (peek().text == "+" || peek().text == "-"))
    {
        BinaryOperator op = parseBinaryOp();
        auto right = parseMultiplicative();

        left = std::make_unique<BinaryExpr>(
            op,
            std::move(left),
            std::move(right));
    }

    return left;
}

std::unique_ptr<Expr> Parser::parseMultiplicative()
{
    auto left = parsePrimary();

    while (isArithmeticOperator(peek()) && (peek().text == "*" || peek().text == "/"))
    {
        BinaryOperator op = parseBinaryOp();
        auto right = parsePrimary();

        left = std::make_unique<BinaryExpr>(
            op,
            std::move(left),
            std::move(right));
    }

    return left;
}

std::unique_ptr<Expr> Parser::parseArithmeticUnary()
{
    if (isUnaryOperator(peek()))
    {
        UnaryOperator op = parseUnaryOp();
        auto operand = parsePrimary();

        return std::make_unique<UnaryExpr>(
            op,
            std::move(operand));
    }

    return parsePrimary();
}

