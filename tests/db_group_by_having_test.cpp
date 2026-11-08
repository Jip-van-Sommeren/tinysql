#include "db_database.h"
#include "db_query_executor.h"
#include "db_test_directory.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{
    using Rows = std::vector<std::vector<Value>>;
    using std::int64_t;
    const Value null = std::monostate{};

    std::string describe(const Rows &rows)
    {
        std::ostringstream output;
        for (const auto &row : rows)
        {
            output << '[';
            for (const auto &value : row)
            {
                std::visit([&output](const auto &inner)
                {
                    using T = std::decay_t<decltype(inner)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                        output << "NULL";
                    else
                        output << inner;
                }, value);
                output << " (type " << value.index() << ") ";
            }
            output << "] ";
        }
        return output.str();
    }

    void requireRows(const QueryResult &result, Rows expected)
    {
        Rows actual;
        for (const auto &row : result.rows)
        {
            if (row.values.size() != result.columns.size())
                throw std::runtime_error("Result row does not match its output schema");
            actual.push_back(row.values);
        }
        // SQL without ORDER BY promises no row order. Preserve duplicates and
        // Value alternatives so incorrect counts and result types still fail.
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        if (!result.returnsRows || result.affectedRows != 0 || actual != expected)
            throw std::runtime_error("Expected " + describe(expected) + "; got " + describe(actual));
    }

    struct QueryCase
    {
        std::string sql;
        Rows expected;
    };

    const std::vector<QueryCase> queries{
        {"SELECT department, COUNT(*), SUM(amount) FROM sales GROUP BY department;",
         {{10, int64_t{3}, int64_t{20}}, {20, int64_t{2}, int64_t{7}}, {null, int64_t{2}, int64_t{5}}}},
        {"SELECT SUM(amount), department, COUNT(*), COUNT(*) FROM sales GROUP BY department;",
         {{int64_t{20}, 10, int64_t{3}, int64_t{3}}, {int64_t{7}, 20, int64_t{2}, int64_t{2}},
          {int64_t{5}, null, int64_t{2}, int64_t{2}}}},
        {"SELECT COUNT(*) FROM sales GROUP BY department;",
         {{int64_t{3}}, {int64_t{2}}, {int64_t{2}}}},
        {"SELECT department, COUNT(*) FROM sales GROUP BY department, department;",
         {{10, int64_t{3}}, {20, int64_t{2}}, {null, int64_t{2}}}},
        {"SELECT department, region, COUNT(*), SUM(amount) FROM sales GROUP BY region, department;",
         {{10, std::string{"east"}, int64_t{2}, int64_t{30}},
          {20, std::string{"east"}, int64_t{2}, int64_t{7}},
          {10, std::string{"west"}, int64_t{1}, int64_t{-10}},
          {null, std::string{"west"}, int64_t{2}, int64_t{5}}}},
        {"SELECT department FROM sales GROUP BY department;", {{10}, {20}, {null}}},
        {"SELECT 7, department + 1 FROM sales GROUP BY department;", {{7, 11}, {7, 21}, {7, null}}},
        {"SELECT department + 1, COUNT(*) FROM sales GROUP BY department + 1;",
         {{11, int64_t{3}}, {21, int64_t{2}}, {null, int64_t{2}}}},
        {"SELECT ABS(amount), COUNT(*), SUM(amount) FROM sales GROUP BY ABS(amount);",
         {{10, int64_t{2}, int64_t{0}}, {20, int64_t{1}, int64_t{20}},
          {7, int64_t{1}, int64_t{7}}, {5, int64_t{1}, int64_t{5}}, {null, int64_t{2}, null}}},
        {"SELECT ABS(amount), COUNT(*) FROM sales GROUP BY amount;",
         {{10, int64_t{1}}, {10, int64_t{1}}, {20, int64_t{1}},
          {7, int64_t{1}}, {5, int64_t{1}}, {null, int64_t{2}}}},
        {"SELECT -amount, COUNT(*) FROM sales GROUP BY -amount;",
         {{-10, int64_t{1}}, {-20, int64_t{1}}, {10, int64_t{1}},
          {-7, int64_t{1}}, {-5, int64_t{1}}, {null, int64_t{2}}}},
        {"SELECT nullable IS NULL, COUNT(*) FROM sales GROUP BY nullable IS NULL;",
         {{true, int64_t{5}}, {false, int64_t{2}}}},
        {"SELECT nullable IS NOT NULL, COUNT(*) FROM sales GROUP BY nullable IS NOT NULL;",
         {{false, int64_t{5}}, {true, int64_t{2}}}},
        {"SELECT COUNT(*) FROM sales GROUP BY nullable IS NULL HAVING nullable IS NULL;",
         {{int64_t{5}}}},
        {"SELECT enabled, COUNT(*) FROM sales GROUP BY enabled;",
         {{true, int64_t{2}}, {false, int64_t{2}}, {null, int64_t{3}}}},
        {"SELECT 'everyone', COUNT(*) FROM sales GROUP BY 'everyone';",
         {{std::string{"everyone"}, int64_t{7}}}},
        {"SELECT COUNT(*) FROM sales GROUP BY NULL;", {{int64_t{7}}}},
        {"SELECT COUNT(*) FROM sales GROUP BY TRUE;", {{int64_t{7}}}},
        {"SELECT COUNT(*) FROM sales GROUP BY 1 + 1;", {{int64_t{7}}}},
        {"SELECT department, COUNT(*), SUM(amount) FROM sales WHERE id > 2 GROUP BY department;",
         {{10, int64_t{1}, int64_t{-10}}, {20, int64_t{2}, int64_t{7}}, {null, int64_t{2}, int64_t{5}}}},
        {"SELECT 12 / (id - 1), COUNT(*) FROM sales WHERE id > 1 GROUP BY 12 / (id - 1);",
         {{12, int64_t{1}}, {6, int64_t{1}}, {4, int64_t{1}}, {3, int64_t{1}}, {2, int64_t{2}}}},
        {"SELECT department, COUNT(*) FROM sales GROUP BY department HAVING COUNT(*) > 2;",
         {{10, int64_t{3}}}},
        {"SELECT department FROM sales GROUP BY department HAVING SUM(amount) > 10;", {{10}}},
        {"SELECT COUNT(*), SUM(amount) FROM sales GROUP BY department "
         "HAVING SUM(amount) > 6 AND COUNT(*) > 1;",
         {{int64_t{3}, int64_t{20}}, {int64_t{2}, int64_t{7}}}},
        {"SELECT SUM(amount) FROM sales GROUP BY region, department "
         "HAVING department = 10 AND region = 'east';", {{int64_t{30}}}},
        {"SELECT department FROM sales GROUP BY department HAVING department = 20;", {{20}}},
        {"SELECT department FROM sales GROUP BY department HAVING department IS NULL;", {{null}}},
        {"SELECT ABS(amount) FROM sales GROUP BY ABS(amount) HAVING ABS(amount) = 10;", {{10}}},
        {"SELECT COUNT(*) FROM sales GROUP BY amount HAVING SUM(amount) IS NULL;", {{int64_t{2}}}},
        {"SELECT COUNT(*) FROM sales GROUP BY enabled HAVING enabled;", {{int64_t{2}}}},
        {"SELECT COUNT(*) FROM sales GROUP BY department HAVING COUNT(*) > 0 AND NULL;", {}},
        {"SELECT COUNT(*) FROM sales GROUP BY department HAVING COUNT(*) > 0 OR NULL;",
         {{int64_t{3}}, {int64_t{2}}, {int64_t{2}}}},
        {"SELECT COUNT(*) FROM sales GROUP BY department HAVING FALSE;", {}},
        {"SELECT COUNT(*) FROM sales GROUP BY department HAVING NULL;", {}},
        {"SELECT 1 / (COUNT(*) - 3) FROM sales GROUP BY department HAVING COUNT(*) <> 3;",
         {{int64_t{-1}}, {int64_t{-1}}}},
        {"SELECT COUNT(*), SUM(amount) FROM sales;", {{int64_t{7}, int64_t{32}}}},
        {"SELECT COUNT(*) FROM sales HAVING COUNT(*) = 7;", {{int64_t{7}}}},
        {"SELECT COUNT(*) FROM sales HAVING COUNT(*) > 7;", {}},
        {"SELECT COUNT(*) FROM sales HAVING NULL;", {}},
        {"SELECT 7 FROM sales HAVING COUNT(*) = 7;", {{7}}},
        {"SELECT 7 FROM sales HAVING COUNT(*) > 7;", {}},
        {"SELECT 7 FROM sales HAVING TRUE;", {{7}}},
        {"SELECT 7 FROM sales HAVING FALSE;", {}},
        {"SELECT 1 / (COUNT(*) - 7) FROM sales HAVING COUNT(*) < 7;", {}},
        {"SELECT department, COUNT(*) FROM sales WHERE id < 0 GROUP BY department;", {}},
        {"SELECT COUNT(*) FROM sales WHERE id < 0 GROUP BY 'everyone';", {}},
        {"SELECT COUNT(*), SUM(amount) FROM sales WHERE id < 0;", {{int64_t{0}, null}}},
        {"SELECT COUNT(*) FROM sales WHERE id < 0 HAVING COUNT(*) = 0;", {{int64_t{0}}}},
        {"SELECT COUNT(*) FROM sales WHERE id < 0 HAVING COUNT(*) > 0;", {}},
        {"SELECT COUNT(*) FROM empty_sales;", {{int64_t{0}}}},
        {"SELECT COUNT(*) FROM empty_sales GROUP BY department;", {}},
        {"SELECT COUNT(*) FROM empty_sales HAVING COUNT(*) = 0;", {{int64_t{0}}}},
        {"SELECT 7 FROM empty_sales HAVING TRUE;", {{7}}},
        {"SELECT d.title, SUM(s.amount) FROM sales AS s "
         "INNER JOIN departments AS d ON s.department = d.code "
         "GROUP BY d.title HAVING SUM(s.amount) > 10;", {{std::string{"alpha"}, int64_t{20}}}}
    };

    struct InvalidQuery
    {
        std::string sql;
        std::string error;
    };

    const std::vector<InvalidQuery> invalidQueries{
        {"SELECT id, COUNT(*) FROM sales;", "GROUP BY"},
        {"SELECT id, COUNT(*) FROM sales GROUP BY department;", "GROUP BY"},
        {"SELECT COUNT(*) FROM sales GROUP BY department HAVING id > 0;", "GROUP BY"},
        {"SELECT COUNT(*) FROM sales HAVING id > 0;", "GROUP BY"},
        {"SELECT amount FROM sales GROUP BY ABS(amount);", "GROUP BY"},
        {"SELECT department + 2 FROM sales GROUP BY department + 1;", "GROUP BY"},
        {"SELECT -amount FROM sales GROUP BY ABS(amount);", "GROUP BY"},
        {"SELECT nullable IS NOT NULL FROM sales GROUP BY nullable IS NULL;", "GROUP BY"},
        {"SELECT id FROM sales WHERE id < 0 GROUP BY department;", "GROUP BY"},
        {"SELECT COUNT(*) FROM sales WHERE id < 0 GROUP BY department HAVING id > 0;", "GROUP BY"},
        {"SELECT COUNT(*) FROM sales GROUP BY COUNT(*);", "Aggregate functions are not allowed"},
        {"SELECT COUNT(*) FROM sales GROUP BY ABS(SUM(amount));", "Aggregate functions are not allowed"},
        {"SELECT department FROM sales WHERE COUNT(*) > 0 GROUP BY department;", "Aggregate functions are not allowed"},
        {"SELECT department FROM sales GROUP BY department HAVING SUM(COUNT(*)) > 0;", "Aggregate functions are not allowed"},
        {"SELECT COUNT(*) FROM sales GROUP BY missing;", "Unknown column"},
        {"SELECT department FROM sales GROUP BY 1;", "Positional references"},
        {"SELECT COUNT(*) FROM sales GROUP BY department HAVING 1;", "Expected a boolean"},
        {"SELECT COUNT(*) FROM sales WHERE id < 0 GROUP BY department HAVING 1;", "Expected a boolean"},
        {"SELECT COUNT(*) FROM empty_sales GROUP BY department HAVING COUNT(*);", "Expected a boolean"}
    };
}

int main()
{
    TestDirectory directory{"db-group-by-having"};
    std::size_t failures = 0;
    {
        auto database = Database::create(directory.path / "db", "group_by_having");
        database.executeSql("CREATE TABLE sales (id INT, amount INT, department INT, "
                            "region TEXT, nullable INT, enabled BOOLEAN);");
        database.executeSql("INSERT INTO sales VALUES "
                            "(1, 10, 10, 'east', NULL, TRUE), (2, 20, 10, 'east', 5, TRUE), "
                            "(3, -10, 10, 'west', NULL, FALSE), (4, 7, 20, 'east', 7, FALSE), "
                            "(5, NULL, 20, 'east', NULL, NULL), (6, 5, NULL, 'west', NULL, NULL), "
                            "(7, NULL, NULL, 'west', NULL, NULL);");
        database.executeSql("CREATE TABLE empty_sales (department INT);");
        database.executeSql("CREATE TABLE departments (code INT, title TEXT);");
        database.executeSql("INSERT INTO departments VALUES (10, 'alpha'), (20, 'beta');");

        for (const auto &[sql, expected] : queries)
        {
            try
            {
                requireRows(database.executeSql(sql), expected);
            }
            catch (const std::exception &error)
            {
                ++failures;
                std::cerr << sql << '\n' << error.what() << '\n';
            }
        }
        for (const auto &[sql, expectedError] : invalidQueries)
        {
            try
            {
                database.executeSql(sql);
                ++failures;
                std::cerr << "Accepted invalid query: " << sql << '\n';
            }
            catch (const std::runtime_error &error)
            {
                if (std::string{error.what()}.find(expectedError) == std::string::npos)
                {
                    ++failures;
                    std::cerr << sql << "\nUnexpected error: " << error.what() << '\n';
                }
            }
        }
    }

    // Reusing a bound query must not mutate its expressions or retain group state.
    auto storage = StorageEngine::open(directory.path / "db");
    Lexer lexer{"SELECT department, COUNT(*) FROM sales GROUP BY department HAVING COUNT(*) > 1;"};
    Parser parser{lexer.tokenize()};
    QueryValidator validator{storage};
    const auto query = validator.validate(parser.parseStatement());
    QueryExecutor executor{storage, "group_by_having"};
    for (int execution = 0; execution < 2; ++execution)
    {
        try
        {
            requireRows(executor.execute(query), {{10, int64_t{3}}, {20, int64_t{2}}, {null, int64_t{2}}});
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "Repeated bound query: " << error.what() << '\n';
        }
    }
    std::cout << queries.size() << " result cases, " << invalidQueries.size()
              << " rejection cases, 2 repeated executions; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
