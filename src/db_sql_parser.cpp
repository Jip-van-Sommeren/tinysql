#include "db_sql_parser.h"

#include <stdexcept>
#include <utility>

ColumnExpr::ColumnExpr(std::vector<std::string> parts)
    : parts(std::move(parts)) {}

NumberExpr::NumberExpr(double value)
    : value(value) {}

StringExpr::StringExpr(std::string value)
    : value(std::move(value)) {}

LogicalExpr::LogicalExpr(
    LogicalOp op,
    std::unique_ptr<Expr> left,
    std::unique_ptr<Expr> right)
    : op(op),
      left(std::move(left)),
      right(std::move(right)) {}

ComparisonExpr::ComparisonExpr(
    ComparisonOp op,
    std::unique_ptr<Expr> left,
    std::unique_ptr<Expr> right)
    : op(op),
      left(std::move(left)),
      right(std::move(right)) {}

ArithmeticExpr::ArithmeticExpr(
    ArithmeticOp op,
    std::unique_ptr<Expr> left,
    std::unique_ptr<Expr> right)
    : op(op),
      left(std::move(left)),
      right(std::move(right)) {}

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
std::unique_ptr<CreateTableStatement> Parser::parseCreateTableStatement(){
    expectKeyword("CREATE");
    expectKeyword("TABLE");
    auto tableNameToken = expect(TokenType::Identifier);
    std::string tableName = std::move(tableNameToken.text);

    auto columns = parseColumnDefinitions();
    finishStatement();
    return std::make_unique<CreateTableStatement>(
        std::move(tableName),
        std::move(columns));
}

std::vector<std::unique_ptr<ColumnDefExpr>> Parser::parseColumnDefinitions(){
    std::vector<std::unique_ptr<ColumnDefExpr>> items;

    items.push_back(parseColumnDefinition());

    while (match(TokenType::Comma))
    {
        items.push_back(parseColumnDefinition());
    }

    return items;
}

std::unique_ptr<ColumnDefExpr> Parser::parseColumnDefinition(){
    Token columnNameToken = expect(TokenType::Identifier);
    std::string columnName = std::move(columnNameToken.text);

    auto dataType = parseDataType();

    std::vector<std::unique_ptr<ConstraintExpr>> constraints;
    if (isColumnConstraintStart())
    {
        constraints = parseColumnConstraints();
        
    }

    return std::make_unique<ColumnDefExpr>(
        std::move(columnName),
        std::move(dataType),
        std::move(constraints));
}

std::unique_ptr<DataTypeExpr> Parser::parseDataType(){
    Token dataTypeToken = expect(TokenType::Keyword);
    std::string dataTypeName = std::move(dataTypeToken.text);

    std::unique_ptr<Expr> length = nullptr;
    if (match(TokenType::LeftParen))
    {
        length = parseExpression();
        expect(TokenType::RightParen);
    }

    return std::make_unique<DataTypeExpr>(
        std::move(dataTypeName),
        std::move(length));
}

std::vector<std::unique_ptr<ConstraintExpr>>
Parser::parseColumnConstraints()
{
    std::vector<std::unique_ptr<ConstraintExpr>> constraints;

    while (
        isColumnConstraintStart()
    )
    {
        constraints.push_back(parseConstraint());
    }

    return constraints;
}

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

std::unique_ptr<ConstraintExpr> Parser::parseConstraint()
{
    std::optional<std::string> name;

    if (matchKeyword("CONSTRAINT"))
    {
        name = parseIdentifier();
    }

    auto constraint = parseConstraintBody();
    constraint->constraintName = std::move(name);

    return constraint;
}


std::unique_ptr<ConstraintExpr> Parser::parseConstraintBody()
{
    if (matchKeyword("DEFAULT"))
    {
        return std::make_unique<ConstraintExpr>(
            ConstraintType::Default,
            parseExpression()
        );
    }

    if (matchKeyword("PRIMARY"))
    {
        expectKeyword("KEY");

        return std::make_unique<ConstraintExpr>(
            ConstraintType::PrimaryKey,
            nullptr
        );
    }

    if (matchKeyword("NOT"))
    {
        expectKeyword("NULL");

        return std::make_unique<ConstraintExpr>(
            ConstraintType::NotNull,
            nullptr
        );
    }

    if (matchKeyword("UNIQUE"))
    {
        return std::make_unique<ConstraintExpr>(
            ConstraintType::Unique,
            nullptr
        );
    }

    if (matchKeyword("NULL"))
    {
        return std::make_unique<ConstraintExpr>(
            ConstraintType::Null,
            nullptr
        );
    }

    if (matchKeyword("CHECK"))
    {
        expect(TokenType::LeftParen);

        auto condition = parseExpression();

        expect(TokenType::RightParen);

        return std::make_unique<ConstraintExpr>(
            ConstraintType::Check,
            std::move(condition)
        );
    }

    throw ParserError(
        "Expected column constraint, found: " + peek().text
    );
}

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

std::unique_ptr<Expr> Parser::parseComparison()
{
    auto left = parsePrimary();

    if (isComparisonOperator(peek()))
    {
        ComparisonOp op = parseComparisonOp();
        auto right = parsePrimary();

        return std::make_unique<ComparisonExpr>(
            op,
            std::move(left),
            std::move(right));
    }
    if (isArithmeticOperator(peek()))
    {
        ArithmeticOp op = parseArithmeticOp();
        auto right = parsePrimary();
        return std::make_unique<ArithmeticExpr>(
            op,
            std::move(left),
            std::move(right));
    }

    return left;
}

ComparisonOp Parser::comparisonOpFromToken(const Token &token)
{
    if (token.type != TokenType::Operator)
    {
        throw std::runtime_error("Incorrect token type was passed");
    }
    if (token.text == "=")
    {
        return ComparisonOp::Eq;
    }
    if (token.text == ">=")
    {
        return ComparisonOp::Ge;
    }
    if (token.text == ">")
    {
        return ComparisonOp::Gt;
    }
    if (token.text == "<=")
    {
        return ComparisonOp::Le;
    }
    if (token.text == "<")
    {
        return ComparisonOp::Lt;
    }
    if (token.text == "<>")
    {
        return ComparisonOp::Ne;
    }
    throw std::runtime_error("Unknown comparison token");
}

ComparisonOp Parser::parseComparisonOp()
{
    Token token = expect(TokenType::Operator);
    return comparisonOpFromToken(token);
}

ArithmeticOp Parser::arithmeticOpFromToken(const Token &token)
{
    if (token.type != TokenType::Operator)
    {
        throw std::runtime_error("Incorrect token type was passed");
    }
    if (token.text == "+")
    {
        return ArithmeticOp::Add;
    }
    if (token.text == "-")
    {
        return ArithmeticOp::Subtract;
    }
    if (token.text == "*")
    {
        return ArithmeticOp::Multiply;
    }
    if (token.text == "/")
    {
        return ArithmeticOp::Divide;
    }
    throw std::runtime_error("Unknown arithmetic token");
}

ArithmeticOp Parser::parseArithmeticOp()
{
    Token token = expect(TokenType::Operator);
    return arithmeticOpFromToken(token);
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
        left = std::make_unique<LogicalExpr>(
            LogicalOp::Or,
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
        left = std::make_unique<LogicalExpr>(
            LogicalOp::And,
            std::move(left),
            std::move(right));
    }

    return left;
}
