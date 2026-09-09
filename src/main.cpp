#include "db_database.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{
    void printValue(const Value &value)
    {
        std::visit(
            [](const auto &innerValue)
            {
                using T = std::decay_t<decltype(innerValue)>;

                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    std::cout << "NULL";
                }
                else
                {
                    std::cout << innerValue;
                }
            },
            value);
    }

    void printRows(const std::vector<ResultRow> &rows)
    {
        for (const ResultRow &row : rows)
        {
            for (std::size_t i = 0; i < row.values.size(); ++i)
            {
                if (i != 0)
                {
                    std::cout << '\t';
                }

                printValue(row.values[i]);
            }

            std::cout << '\n';
        }
    }

    void printQueryResult(const QueryResult &result)
    {
        if (result.returnsRows)
        {
            for (std::size_t i = 0; i < result.columns.size(); ++i)
            {
                if (i != 0)
                {
                    std::cout << '\t';
                }
                std::cout << result.columns[i].name;
            }
            std::cout << '\n';
            printRows(result.rows);
            std::cout << result.rows.size() << " row(s) selected\n";
            return;
        }

        std::cout << result.affectedRows << " row(s) affected\n";
    }
}

int main()
{
    std::filesystem::path dbPath{"/tmp/query_test_db"};
    std::filesystem::remove_all(dbPath);

    Database db{dbPath, "query_test_db"};
    std::vector<Column> columns{
        Column{
            .name = "a",
            .type = DataType::Int,
            .nullable = false,
            .columnIndex = 0,
            .storage = FixedColumnStorage{}},
        Column{
            .name = "b",
            .type = DataType::Text,
            .nullable = false,
            .columnIndex = 1,
            .storage = VarColumnStorage{}},
        Column{
            .name = "c",
            .type = DataType::Text,
            .nullable = true,
            .columnIndex = 2,
            .storage = VarColumnStorage{}}};
    std::vector<Constraint> constraints;
    db.createTable("test", columns, constraints);

    std::vector<std::string> queries{
        "INSERT INTO test (a, b, c) VALUES (1, 'Appel', 'Peer');",
        "INSERT INTO test (a, b, c) VALUES (2, 'Mandarijn', 'Banaan');",
        "INSERT INTO test (a, b, c) VALUES (3, 'Kiwi', NULL);",
        "SELECT * FROM test;",
        "SELECT a, b FROM test WHERE a >= 2;",
        "SELECT b, c FROM test WHERE c IS NULL;",
        "SELECT test.* FROM test WHERE b >= 'Mandarijn';",
        "SELECT a, c FROM test WHERE a > 1 AND c IS NOT NULL;"};

    for (const std::string &query : queries)
    {
        std::cout << "SQL> " << query << '\n';
        printQueryResult(db.executeSql(query));
        std::cout << '\n';
    }
}
