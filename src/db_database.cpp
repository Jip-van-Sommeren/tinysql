#include "db_database.h"

#include "db_query_executor.h"
#include "db_sql_lexer.h"
#include "db_sql_parser.h"

#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

Database::Database(StorageEngine engine, const std::string &name)
    : dbName(name), storageEngine(std::move(engine))

{
}

Database Database::create(
    const std::filesystem::path &path, const std::string &name)
{
    // Create the database root before creating its subdirectories.

    return Database{StorageEngine::create(path), name};
}

Database Database::open(
    const std::filesystem::path &path, const std::string &name)
{
    // Opening either missing subdirectory throws; nothing is created.

    {
        return Database{
            StorageEngine::open(path),
            name};
    }
}

QueryResult Database::executeSql(const std::string &sql)
{
    storageEngine.requireReadable();
    Lexer lexer(sql);
    Parser parser(lexer.tokenize());
    Statement statement = parser.parseStatement();

    QueryValidator validator{storageEngine};
    BoundQuery query = validator.validate(statement);

    QueryExecutor executor{storageEngine, dbName};
    return executor.execute(query);
}
