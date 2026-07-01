#include "db_sql_lexer.h"

#include <cctype>
#include <stdexcept>
#include <utility>

Lexer::Lexer(std::string input)
    : input(std::move(input)), pos(0) {}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;

    while (!isAtEnd())
    {
        char c = peek();

        if (std::isspace(static_cast<unsigned char>(c)))
        {
            skipWhitespace();
        }
        else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            tokens.push_back(readWord());
        }
        else if (std::isdigit(static_cast<unsigned char>(c)))
        {
            tokens.push_back(readNumber());
        }
        else if (c == '\'')
        {
            tokens.push_back(readString());
        }
        else if (c == ',')
        {
            advance();
            tokens.push_back({TokenType::Comma, ","});
        }
        else if (c == ';')
        {
            advance();
            tokens.push_back({TokenType::Semicolon, ";"});
        }
        else if (c == '(')
        {
            advance();
            tokens.push_back({TokenType::LeftParen, "("});
        }
        else if (c == ')')
        {
            advance();
            tokens.push_back({TokenType::RightParen, ")"});
        }
        else if (c == '.')
        {
            advance();
            tokens.push_back({TokenType::Dot, "."});
        }
        else if (isOperatorStart(c))
        {
            tokens.push_back(readOperator());
        }
        else
        {
            throw std::runtime_error(std::string("Unexpected character: ") + c);
        }
    }

    tokens.push_back({TokenType::End, ""});
    return tokens;
}

bool Lexer::isAtEnd() const
{
    return pos >= input.size();
}

char Lexer::peek() const
{
    return input[pos];
}

char Lexer::advance()
{
    return input[pos++];
}

void Lexer::skipWhitespace()
{
    while (!isAtEnd() && std::isspace(static_cast<unsigned char>(peek())))
    {
        advance();
    }
}

Token Lexer::readWord()
{
    std::size_t start = pos;

    while (!isAtEnd() &&
           (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_'))
    {
        advance();
    }

    std::string text = input.substr(start, pos - start);

    if (isKeyword(text))
    {
        return {TokenType::Keyword, text};
    }

    return {TokenType::Identifier, text};
}

Token Lexer::readNumber()
{
    std::size_t start = pos;

    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
    {
        advance();
    }

    if (!isAtEnd() && peek() == '.')
    {
        advance();

        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        {
            advance();
        }
    }

    return {TokenType::Number, input.substr(start, pos - start)};
}

Token Lexer::readString()
{
    advance(); // consume opening '

    std::size_t start = pos;

    while (!isAtEnd() && peek() != '\'')
    {
        advance();
    }

    if (isAtEnd())
    {
        throw std::runtime_error("Unterminated string literal");
    }

    std::string text = input.substr(start, pos - start);

    advance(); // consume closing '

    return {TokenType::String, text};
}

Token Lexer::readOperator()
{
    char c = advance();

    if (!isAtEnd())
    {
        char next = peek();

        if ((c == '>' && next == '=') ||
            (c == '<' && next == '=') ||
            (c == '!' && next == '=') ||
            (c == '<' && next == '>'))
        {
            advance();
            return {TokenType::Operator, std::string{c} + next};
        }
    }

    return {TokenType::Operator, std::string{c}};
}

bool Lexer::isOperatorStart(char c) const
{
    return c == '=' || c == '>' || c == '<' || c == '!' ||
           c == '+' || c == '-' || c == '*' || c == '/';
}

bool Lexer::isKeyword(const std::string &word) const
{
    return word == "SELECT" ||
           word == "FROM" ||
           word == "WHERE" ||
           word == "INSERT" ||
           word == "INTO" ||
           word == "VALUES" ||
           word == "UPDATE" ||
           word == "SET" ||
           word == "DELETE" ||
           word == "NULL" ||
           word == "AND" ||
           word == "IS" ||
           word == "NOT" ||
           word == "ON" ||
           word == "INNER" ||
           word == "OUTER" ||
           word == "CROSS" ||
           word == "LEFT" ||
           word == "RIGHT" ||
           word == "FULL" ||
           word == "JOIN" ||
           word == "OR";
}
