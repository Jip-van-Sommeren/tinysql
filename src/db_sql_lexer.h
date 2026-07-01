#pragma once

#include <cstddef>
#include <string>
#include <vector>

enum class TokenType
{
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

struct Token
{
    TokenType type;
    std::string text;
};

class Lexer
{
public:
    explicit Lexer(std::string input);

    std::vector<Token> tokenize();

private:
    std::string input;
    std::size_t pos;

    bool isAtEnd() const;
    char peek() const;
    char advance();
    void skipWhitespace();
    Token readWord();
    Token readNumber();
    Token readString();
    Token readOperator();
    bool isOperatorStart(char c) const;
    bool isKeyword(const std::string &word) const;
};
