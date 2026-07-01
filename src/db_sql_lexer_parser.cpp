#include "db_sql_lexer.h"
#include "db_sql_parser.h"

#include <memory>
#include <string>
#include <vector>

int main()
{
    std::vector<std::string> sqlStatements = {
        "SELECT users.* FROM users WHERE age >= 18;",
        "SELECT * FROM users JOIN orders ON users.id = orders.user_id;",
        "DELETE FROM users WHERE age < 18;",
        "INSERT INTO users (name, age) VALUES ('Ada', 37);",
        "UPDATE users SET age = age + 1 WHERE name = 'Ada';"};

    for (const std::string &sql : sqlStatements)
    {
        Lexer lexer(sql);
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(tokens);
        std::unique_ptr<Statement> stmt = parser.parseStatement();
        (void)stmt;
    }
}
