#include "db_database.h"
#include "db_query_exception.h"
#include "db_test_directory.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    QueryResult execute(Database &database, const std::string &sql)
    {
        auto result = database.tryExecuteSql(sql);
        if (!result)
            throw std::runtime_error(sql + ": " + result.error().message);
        return std::move(*result);
    }

    void reject(Database &database, const std::string &sql,
                QueryErrorCode code, const std::string &message)
    {
        auto result = database.tryExecuteSql(sql);
        require(!result, "Query unexpectedly succeeded: " + sql);
        require(result.error().code == code, "Wrong error category: " + sql);
        require(result.error().message.find(message) != std::string::npos,
                "Unexpected error message: " + result.error().message);
    }

    void testQueryRejections()
    {
        TestDirectory directory{"db-query-rejections"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        execute(database, "CREATE TABLE items (id INT PRIMARY KEY, value INT NOT NULL, label TEXT);");
        execute(database, "INSERT INTO items VALUES (1, 10, 'original');");

        struct Rejection
        {
            std::string sql;
            QueryErrorCode code;
            std::string message;
        };
        const Rejection rejections[]{
            {"SELECT @ FROM items;", QueryErrorCode::Syntax, "Unexpected character"},
            {"SELECT 'unfinished", QueryErrorCode::Syntax, "Unterminated string"},
            {"SELECT FROM items;", QueryErrorCode::Syntax, "Expected expression"},
            {"SELECT * FROM missing;", QueryErrorCode::Validation, "Table does not exist"},
            {"SELECT missing FROM items;", QueryErrorCode::Validation, "Unknown column"},
            {"INSERT INTO items VALUES (2, 'wrong type', NULL);", QueryErrorCode::Validation, "Expected numeric"},
            {"CREATE TABLE items (id INT);", QueryErrorCode::Validation, "Table already exists"},
            {"SELECT ABS() FROM items;", QueryErrorCode::Validation, "ABS requires"},
            {"CREATE TABLE unsupported_check (id INT CHECK (ABS(id) > 0));",
             QueryErrorCode::Validation, "Stored function expressions are not supported"},
            {"SELECT id, COUNT(*) FROM items;", QueryErrorCode::Validation, "requires GROUP BY"},
            {"SELECT * FROM items WHERE id;", QueryErrorCode::Validation, "Expected a boolean"},
            {"SELECT DATE '2024-02-30' FROM items;", QueryErrorCode::Validation, "Invalid date"},
            {"INSERT INTO items VALUES (1, 20, NULL);", QueryErrorCode::Constraint, "PRIMARY KEY"},
            {"INSERT INTO items VALUES (2, NULL, NULL);", QueryErrorCode::Constraint, "NOT NULL"},
            {"SELECT 999999999999999999999999 FROM items;", QueryErrorCode::Numeric, "Integer literal is out of range"},
            {"SELECT 2147483647 + 1 FROM items;", QueryErrorCode::Numeric, "overflow"},
            {"SELECT value + 2147483647 FROM items;", QueryErrorCode::Numeric, "overflow"},
            {"SELECT 1 / 0 FROM items;", QueryErrorCode::Numeric, "Division by zero"},
            {"SELECT value / (id - 1) FROM items;", QueryErrorCode::Numeric, "Division by zero"},
            {"SELECT 1.0 / 3.0 FROM items;", QueryErrorCode::Numeric, "no exact result"},
            {"SELECT 0.1234567890123456789 FROM items;", QueryErrorCode::Numeric, "scale exceeds"},
            {"UPDATE items SET value = value / (id - 1);", QueryErrorCode::Numeric, "Division by zero"},
            {"UPDATE items SET value = 'wrong type';", QueryErrorCode::Validation, "expects type"},
            {"INSERT INTO items VALUES (2, 20, '" + std::string(70000, 'x') + "');",
             QueryErrorCode::Validation, "Row is too large"},
        };

        for (const auto &rejection : rejections)
        {
            reject(database, rejection.sql, rejection.code, rejection.message);
            require(!std::filesystem::exists(root / "journal/journal.log"),
                    "Rejected query left an active journal");
            const auto rows = execute(database, "SELECT id, value, label FROM items;").rows;
            require(rows.size() == 1 && std::get<std::int32_t>(rows[0].values[1]) == 10 &&
                        std::get<std::string>(rows[0].values[2]) == "original",
                    "Rejected query changed stored rows");
            execute(database, "UPDATE items SET value = value WHERE id = 1;");
        }
    }

    void testRollbackAfterIndexRejection()
    {
        TestDirectory directory{"db-query-index-rejection"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        execute(database, "CREATE TABLE items (id INT PRIMARY KEY, label TEXT);");
        execute(database, "CREATE UNIQUE INDEX labels ON items (label);");
        execute(database, "INSERT INTO items VALUES (1, 'original');");

        // Table rows and the first index key have already been written when
        // the second key fails. Both must be undone before returning an error.
        reject(database, "INSERT INTO items VALUES (2, 'fresh'), (3, 'original');",
               QueryErrorCode::Constraint, "UNIQUE INDEX");
        require(!std::filesystem::exists(root / "journal/journal.log"), "Rollback left an active journal");
        require(execute(database, "SELECT * FROM items;").rows.size() == 1,
                "Rejected indexed insert retained rows");
        execute(database, "INSERT INTO items VALUES (2, 'fresh');");
        reject(database, "UPDATE items SET label = 'original' WHERE id = 2;",
               QueryErrorCode::Constraint, "UNIQUE INDEX");
        const auto result = execute(database, "SELECT label FROM items WHERE id = 2;");
        require(std::get<std::string>(result.rows.at(0).values.at(0)) == "fresh",
                "Rejected indexed update retained its changes");

        execute(database, "CREATE TABLE duplicates (id INT, label TEXT);");
        execute(database, "INSERT INTO duplicates VALUES (1, 'same'), (2, 'same');");
        reject(database, "CREATE UNIQUE INDEX unique_labels ON duplicates (label);",
               QueryErrorCode::Constraint, "UNIQUE INDEX");
        require(!std::filesystem::exists(root / "indexes/unique_labels.index"),
                "Rejected index creation retained the index file");
        execute(database, "UPDATE duplicates SET label = 'different' WHERE id = 2;");
        execute(database, "CREATE UNIQUE INDEX unique_labels ON duplicates (label);");
    }

    void testStorageFailureIsNotAQueryRejection()
    {
        TestDirectory directory{"db-query-fatal-error"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        execute(database, "CREATE TABLE items (id INT);");
        std::filesystem::resize_file(root / "tables/items.table", 1);

        bool threw = false;
        try
        {
            static_cast<void>(database.tryExecuteSql("SELECT * FROM items;"));
        }
        catch (const QueryException &)
        {
            throw std::runtime_error("Storage corruption was classified as a query rejection");
        }
        catch (const std::exception &)
        {
            threw = true;
        }
        require(threw, "Storage corruption was swallowed by the query API");
    }
}

int main() try
{
    testQueryRejections();
    testRollbackAfterIndexRejection();
    testStorageFailureIsNotAQueryRejection();
}
catch (const std::exception &error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
