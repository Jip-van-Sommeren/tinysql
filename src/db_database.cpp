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

Database::Database(StorageEngine engine, std::string name)
    : dbName(std::move(name)),
      storageEngine(std::move(engine))
{
}

Database Database::create(
    const std::filesystem::path &path, std::string name)
{
    // Create the database root before creating its subdirectories.

    return Database{StorageEngine::create(path),
                    std::move(name)};
}

Database Database::open(
    const std::filesystem::path &path, std::string name)
{
    // Opening either missing subdirectory throws; nothing is created.

    {
        return Database{
            StorageEngine::open(path),
            std::move(name)};
    }
}

void Database::createTable(
    const std::string &tableName,
    const std::vector<Column> &columns,
    const std::vector<Constraint> &constraints)
{
    storageEngine.createTable(tableName, dbName, columns, constraints);
}

void Database::insertRows(
    const std::string &tableName,
    const std::vector<Row> &rows)
{
    Table table = storageEngine.openTable(tableName);
    table.insertRows(BoundInsert{
        .tableName = tableName,
        .rows = rows});
}

std::vector<Row> Database::selectAllRows(const std::string &tableName)
{
    Table table = storageEngine.openTable(tableName);
    TableCursor cursor = table.scan();
    std::vector<Row> rows;
    while (auto row = cursor.next())
    {
        rows.push_back(std::move(*row));
    }
    return rows;
}

QueryResult Database::execute(const BoundQuery &query)
{
    return std::visit(
        [this](const auto &boundQuery) -> QueryResult
        {
            using T = std::decay_t<decltype(boundQuery)>;

            if constexpr (std::is_same_v<T, BoundInsert>)
            {
                return executeInsert(boundQuery);
            }
            else if constexpr (std::is_same_v<T, BoundSelect>)
            {
                return executeSelect(boundQuery);
            }
            else if constexpr (std::is_same_v<T, BoundDelete>)
            {
                return executeDelete(boundQuery);
            }
            else
            {
                return executeCreateTable(boundQuery);
            }
        },
        query);
}

QueryResult Database::executeSql(const std::string &sql)
{
    Lexer lexer(sql);
    Parser parser(lexer.tokenize());
    std::unique_ptr<Statement> statement = parser.parseStatement();

    QueryValidator validator{storageEngine};
    BoundQuery query = validator.validate(*statement);

    return execute(query);
}

QueryResult Database::executeInsert(const BoundInsert &insert)
{
    Table table = storageEngine.openTable(insert.tableName);
    table.insertRows(insert);

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = insert.rows.size(),
        .returnsRows = false};
}

QueryResult Database::executeSelect(const BoundSelect &select)
{
    Table table = storageEngine.openTable(select.tableName);
    SelectExecutor executor;
    return executor.execute(select, table);
}

QueryResult Database::executeDelete(const BoundDelete &del)
{
    Table table = storageEngine.openTable(del.tableName);
    std::uint64_t deletedCount = table.deleteRows(del);

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = deletedCount,
        .returnsRows = false};
}

QueryResult Database::executeCreateTable(
    const BoundCreateTable &createTable)
{
    storageEngine.createTable(
        createTable.tableName,
        dbName,
        createTable.columns,
        createTable.constraints);

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = 0,
        .returnsRows = false};
}
