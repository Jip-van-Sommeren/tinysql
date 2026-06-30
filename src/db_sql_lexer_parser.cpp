#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <optional>
#include <vector>

enum class TokenType {
    Keyword,
    Identifier,
    Number,
    String,

    Comma,
    Semicolon,
    LeftParen,
    RightParen,

    Operator,
    Star,
    Dot,

    End
};

struct Token {
    TokenType type;
    std::string text;
};

class Lexer {
public:
    explicit Lexer(std::string input)
        : input(std::move(input)), pos(0) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;

        while (!isAtEnd()) {
            char c = peek();

            if (std::isspace(static_cast<unsigned char>(c))) {
                skipWhitespace();
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                tokens.push_back(readWord());
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                tokens.push_back(readNumber());
            } else if (c == '\'') {
                tokens.push_back(readString());
            } else if (c == ',') {
                advance();
                tokens.push_back({TokenType::Comma, ","});
            } else if (c == ';') {
                advance();
                tokens.push_back({TokenType::Semicolon, ";"});
            } else if (c == '(') {
                advance();
                tokens.push_back({TokenType::LeftParen, "("});
            } else if (c == ')') {
                advance();
                tokens.push_back({TokenType::RightParen, ")"});
            } else if (isOperatorStart(c)) {
                tokens.push_back(readOperator());
            } else {
                throw std::runtime_error(std::string("Unexpected character: ") + c);
            }
        }

        tokens.push_back({TokenType::End, ""});
        return tokens;
    }

private:
    std::string input;
    size_t pos;

    bool isAtEnd() const {
        return pos >= input.size();
    }

    char peek() const {
        return input[pos];
    }

    char peekNext() const {
        if (pos + 1 >= input.size()) {
            return '\0';
        }
        return input[pos + 1];
    }

    char advance() {
        return input[pos++];
    }

    void skipWhitespace() {
        while (!isAtEnd() && std::isspace(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    Token readWord() {
        size_t start = pos;

        while (!isAtEnd() &&
               (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
            advance();
        }

        std::string text = input.substr(start, pos - start);

        if (isKeyword(text)) {
            return {TokenType::Keyword, text};
        }

        return {TokenType::Identifier, text};
    }

    Token readNumber() {
        size_t start = pos;

        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }

        if (!isAtEnd() && peek() == '.') {
            advance();

            while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }

        return {TokenType::Number, input.substr(start, pos - start)};
    }

    Token readString() {
        advance(); // consume opening '

        size_t start = pos;

        while (!isAtEnd() && peek() != '\'') {
            advance();
        }

        if (isAtEnd()) {
            throw std::runtime_error("Unterminated string literal");
        }

        std::string text = input.substr(start, pos - start);

        advance(); // consume closing '

        return {TokenType::String, text};
    }

    Token readOperator() {
        char c = advance();

        if (!isAtEnd()) {
            char next = peek();

            if ((c == '>' && next == '=') ||
                (c == '<' && next == '=') ||
                (c == '!' && next == '=') ||
                (c == '<' && next == '>')) {
                advance();
                return {TokenType::Operator, std::string{c} + next};
            }
        }

        return {TokenType::Operator, std::string{c}};
    }

    bool isOperatorStart(char c) const {
        return c == '=' || c == '>' || c == '<' || c == '!' ||
               c == '+' || c == '-' || c == '*' || c == '/';
    }

    bool isKeyword(const std::string& word) const {
        return word == "SELECT" ||
               word == "FROM" ||
               word == "WHERE" ||
               word == "INSERT" ||
               word == "UPDATE" ||
               word == "DELETE" ||
               word == "AND" ||
               word == "IS" ||
               word == "NOT" ||

               word == "OR";
    }
};



struct Expr {
    virtual ~Expr() = default;
};

struct ColumnExpr : Expr {
    std::vector<std::string> parts;

    explicit ColumnExpr(std::vector<std::string> parts)
        : parts(std::move(parts)) {}
};

struct NumberExpr : Expr {
    double value;

    explicit NumberExpr(double value)
        : value(value) {}
};

struct StringExpr : Expr {
    std::string value;

    explicit StringExpr(std::string value)
        : value(std::move(value)) {}
};

struct BinaryExpr : Expr {
    std::string op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    BinaryExpr(
        std::string op,
        std::unique_ptr<Expr> left,
        std::unique_ptr<Expr> right
    )
        : op(std::move(op)),
          left(std::move(left)),
          right(std::move(right)) {}
};

enum class LogicalOp{
    And,
    Or
};

enum class ComparisonOp{
    Gt,
    Ge,
    Lt,
    Le,
    Eq,
    Ne
};

enum class ArithmeticOp {
    Add,
    Subtract,
    Multiply,
    Divide
};

struct LogicalExpr : Expr {
    LogicalOp op; // And / Or
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

struct ComparisonExpr : Expr {
    ComparisonOp op; // Equal / Greater / Less / etc.
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

struct ArithmicExpr : Expr {
    ArithmeticOp op; // Equal / Greater / Less / etc.
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

struct SelectItem {
    virtual ~SelectItem() = default;
};

struct WildcardSelectItem : SelectItem {
    // SELECT *
};

struct QualifiedWildcardSelectItem : SelectItem {
    std::vector<std::string> qualifierParts;

    explicit QualifiedWildcardSelectItem(std::vector<std::string> qualifierParts)
        : qualifierParts(std::move(qualifierParts)) {}
};

struct ExprSelectItem : SelectItem {
    std::unique_ptr<Expr> expr;

    explicit ExprSelectItem(std::unique_ptr<Expr> expr)
        : expr(std::move(expr)) {}
};

struct SelectStatement {
    std::vector<std::unique_ptr<SelectItem>> selectList;
    std::string tableName;
    std::unique_ptr<Expr> where;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens)
        : tokens(std::move(tokens)), pos(0) {}

    SelectStatement parseSelectStatement() {
        SelectStatement stmt;

        expectKeyword("SELECT");

        stmt.selectList = parseSelectList();

        expectKeyword("FROM");

        Token table = expect(TokenType::Identifier);
        stmt.tableName = table.text;

        if (matchKeyword("WHERE")) {
            stmt.where = parseExpression();
        }

        match(TokenType::Semicolon);

        expect(TokenType::End);

        return stmt;
    }

private:
    std::vector<Token> tokens;
    size_t pos;

    bool checkNext(TokenType type) const {
        if (pos + 1 >= tokens.size()) {
            return false;
        }

        return tokens[pos + 1].type == type;
    }

    bool checkNextNext(TokenType type) const {
        if (pos + 2 >= tokens.size()) {
            return false;
        }

        return tokens[pos + 2].type == type;
    }

    const Token& peek() const {
        return tokens[pos];
    }

    const Token& previous() const {
        return tokens[pos - 1];
    }

    bool isAtEnd() const {
        return peek().type == TokenType::End;
    }

    const Token& advance() {
        if (!isAtEnd()) {
            pos++;
        }

        return previous();
    }

    bool check(TokenType type) const {
        return peek().type == type;
    }

    bool checkKeyword(const std::string& keyword) const {
        return peek().type == TokenType::Keyword && peek().text == keyword;
    }

    bool match(TokenType type) {
        if (check(type)) {
            advance();
            return true;
        }

        return false;
    }

    bool matchKeyword(const std::string& keyword) {
        if (checkKeyword(keyword)) {
            advance();
            return true;
        }

        return false;
    }

    Token expect(TokenType type) {
        if (!check(type)) {
            throw std::runtime_error("Unexpected token: " + peek().text);
        }

        return advance();
    }

    void expectKeyword(const std::string& keyword) {
        if (!matchKeyword(keyword)) {
            throw std::runtime_error("Expected keyword: " + keyword);
        }
    }



    std::unique_ptr<SelectItem> parseSelectItem() {
        // SELECT *
        if (match(TokenType::Star)) {
            return std::make_unique<WildcardSelectItem>();
        }
        

        if (check(TokenType::Identifier)) {
            size_t savedPos = pos;

            std::vector<std::string> parts;
            parts.push_back(advance().text);

            while (match(TokenType::Dot)) {
                if (match(TokenType::Star)) {
                    return std::make_unique<QualifiedWildcardSelectItem>(
                        std::move(parts)
                    );
                }

                Token name = expect(TokenType::Identifier);
                parts.push_back(name.text);
            }

            pos = savedPos;
        }

        // SELECT name
        // SELECT price * quantity
        auto expr = parseExpression();

        return std::make_unique<ExprSelectItem>(
            std::move(expr)
        );
    }
    std::vector<std::unique_ptr<SelectItem>> parseSelectList() {
        std::vector<std::unique_ptr<SelectItem>> items;

        items.push_back(parseSelectItem());


        while (match(TokenType::Comma)) {
            items.push_back(parseSelectItem());
        }

        return items;
    }


    bool isComparisonOperator(const Token& token) {
        if (token.type != TokenType::Operator) {
            return false;
        }

        return token.text == "="  ||
            token.text == "<>" ||
            token.text == "<"  ||
            token.text == "<=" ||
            token.text == ">"  ||
            token.text == ">=";
    }

    bool isArithmicOperator(const Token& token) {
        if (token.type != TokenType::Operator) {
            return false;
        }

        return token.text == "+"  ||
            token.text == "-" ||
            token.text == "*"  ||
            token.text == "/";
    }


    std::unique_ptr<Expr> parseComparison() {
        auto left = parsePrimary();

        if (isComparisonOperator(peek())) {
            ComparisonOp op = parseComparisonOp();
            auto right = parsePrimary();

            return std::make_unique<ComparisonExpr>(
                op,
                std::move(left),
                std::move(right)
            );
        }
        if (isArithmicOperator(peek())) {
            ArithmeticOp op = parseArithmeticOp();
            auto right = parsePrimary();
            return std::make_unique<ArithmicExpr>(
                op,
                std::move(left),
                std::move(right)
            );
        }


        return left;
    }




    ComparisonOp comparisonOpFromToken(const Token &token){
        if (token.type != TokenType::Operator){
            throw std::runtime_error("Incorrect token type was passed");
        }
        if (token.text == "="){
            return ComparisonOp::Eq;
        }
        if (token.text == ">="){
            return ComparisonOp::Ge;
        }
        if (token.text == ">"){
            return ComparisonOp::Gt;
        }        
        if (token.text == "<="){
            return ComparisonOp::Le;
        }        
        if (token.text == "<"){
            return ComparisonOp::Lt;
        }        
        if (token.text == "<>"){
            return ComparisonOp::Ne;
        }
    }

    ComparisonOp parseComparisonOp() {
        Token token = expect(TokenType::Operator);
        return comparisonOpFromToken(token);
    }

    ArithmeticOp arithmeticOpFromToken(const Token &token){
        if (token.type != TokenType::Operator){
            throw std::runtime_error("Incorrect token type was passed");
        }
        if (token.text == "="){
            return ArithmeticOp::Multiply;
        }
        if (token.text == ">="){
            return ArithmeticOp::Add;
        }
        if (token.text == ">"){
            return ArithmeticOp::Subtract;
        }        
        if (token.text == "<="){
            return ArithmeticOp::Divide;
        }        
        throw std::runtime_error("Unknown arithmic token");

    }

    ArithmeticOp parseArithmeticOp() {
        Token token = expect(TokenType::Operator);
        return arithmeticOpFromToken(token);
    }

    std::unique_ptr<Expr> parsePrimary() {
        if (check(TokenType::Identifier)) {
            std::vector<std::string> parts;

            parts.push_back(advance().text);

            while (match(TokenType::Dot)) {
                Token name = expect(TokenType::Identifier);
                parts.push_back(name.text);
            }

            return std::make_unique<ColumnExpr>(std::move(parts));
        }

        if (check(TokenType::Number)) {
            double value = std::stod(advance().text);
            return std::make_unique<NumberExpr>(value);
        }

        if (check(TokenType::String)) {
            std::string value = advance().text;
            return std::make_unique<StringExpr>(value);
        }

        throw std::runtime_error("Expected expression, got: " + peek().text);
    }
    std::unique_ptr<Expr> parseExpression() {
        return parseOr();
    }

    std::unique_ptr<Expr> parseOr() {
        auto left = parseAnd();

        while (matchKeyword("OR")) {
            auto right = parseAnd();
            left = std::make_unique<LogicalExpr>("OR", std::move(left), std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expr> parseAnd() {
        auto left = parseComparison();

        while (matchKeyword("AND")) {
            auto right = parseComparison();
            left = std::make_unique<LogicalExpr>("AND", std::move(left), std::move(right));
        }

        return left;
    }
};

int main(){
    std::string sql = "SELECT name, age FROM users WHERE age >= 18;";

    Lexer lexer(sql);
    std::vector<Token> tokens = lexer.tokenize();

    for (const Token& token : tokens) {
        std::cout << token.text << "\n";
    }
}
