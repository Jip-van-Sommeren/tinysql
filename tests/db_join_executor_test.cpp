#include "db_database.h"
#include "db_test_directory.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    using Values = std::vector<Value>;
    using Rows = std::vector<Values>;
    const Value null = std::monostate{};

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    QueryResult expectRows(Database &database, const std::string &sql, const Rows &expected)
    {
        auto result = database.executeSql(sql);
        require(result.returnsRows && result.affectedRows == 0,
                "JOIN must return rows without reporting writes: " + sql);
        require(result.rows.size() == expected.size(),
                "Expected " + std::to_string(expected.size()) + " rows, got " +
                    std::to_string(result.rows.size()) + ": " + sql);

        Rows actual;
        for (const auto &row : result.rows)
        {
            require(row.values.size() == result.columns.size(),
                    "Row width differs from the output schema: " + sql);
            actual.push_back(row.values);
        }
        // SQL without ORDER BY does not promise row order. Preserve duplicate
        // multiplicity while comparing values, including their physical types.
        require(std::is_permutation(actual.begin(), actual.end(), expected.begin(), expected.end()),
                "Unexpected row values or duplicate multiplicity: " + sql);
        return result;
    }

    void setup(Database &database)
    {
        database.executeSql("CREATE TABLE left_items (id INT, join_key INT);");
        database.executeSql("CREATE TABLE right_items (id INT, join_key INT, payload INT);");
        // The first left row has no match. Right rows 101 and 102 only match
        // later left rows, so they must not be emitted as unmatched early.
        database.executeSql("INSERT INTO left_items VALUES (1, 10), (2, 20), (3, 20), (4, 30), (5, NULL);");
        database.executeSql("INSERT INTO right_items VALUES (101, 20, 7), (102, 20, 8), (103, 40, 9), (104, NULL, 10);");
        database.executeSql("CREATE TABLE empty_left (id INT, join_key INT);");
        database.executeSql("CREATE TABLE empty_right (id INT, join_key INT, payload INT);");
        database.executeSql("CREATE TABLE duplicate_left (id INT, join_key INT);");
        database.executeSql("CREATE TABLE duplicate_right (id INT, join_key INT);");
        database.executeSql("INSERT INTO duplicate_left VALUES (1, 7), (1, 7);");
        database.executeSql("INSERT INTO duplicate_right VALUES (2, 7), (2, 7), (3, 8), (3, 8);");
        database.executeSql("CREATE TABLE annotations (right_id INT, tag TEXT);");
        database.executeSql("INSERT INTO annotations VALUES (101, 'first'), (103, 'orphan'), (999, 'only');");
    }

    void testInnerAndCross(Database &database)
    {
        const Rows matches{{2, 101}, {2, 102}, {3, 101}, {3, 102}};
        expectRows(database,
                   "SELECT l.id, r.id FROM left_items AS l JOIN right_items AS r ON l.join_key = r.join_key;",
                   matches);
        expectRows(database,
                   "SELECT l.id, r.id FROM left_items AS l INNER JOIN right_items AS r ON l.join_key = r.join_key;",
                   matches);
        expectRows(database,
                   "SELECT l.id, r.id FROM left_items AS l CROSS JOIN right_items AS r;",
                   {{1, 101}, {1, 102}, {1, 103}, {1, 104},
                    {2, 101}, {2, 102}, {2, 103}, {2, 104},
                    {3, 101}, {3, 102}, {3, 103}, {3, 104},
                    {4, 101}, {4, 102}, {4, 103}, {4, 104},
                    {5, 101}, {5, 102}, {5, 103}, {5, 104}});

        auto result = expectRows(database,
            "SELECT r.*, l.id AS left_id FROM left_items AS l "
            "JOIN right_items AS r ON l.join_key = r.join_key WHERE l.id = 2;",
            {{101, 20, 7, 2}, {102, 20, 8, 2}});
        require(result.columns.size() == 4 && result.columns[0].name == "id" &&
                    result.columns[1].name == "join_key" && result.columns[2].name == "payload" &&
                    result.columns[3].name == "left_id",
                "Qualified wildcard or projection alias has the wrong output schema");
        result = expectRows(database,
            "SELECT l.id + r.payload AS total FROM left_items AS l "
            "JOIN right_items AS r ON l.join_key = r.join_key WHERE l.id = 2 AND r.id = 102;",
            {{10}});
        require(result.columns[0].name == "total" && result.columns[0].type == DataType::Int,
                "Expression over both inputs has the wrong output metadata");
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l JOIN right_items AS r "
            "ON l.id < r.payload WHERE l.id = 1 AND r.id = 101;",
            {{1, 101}});
    }

    void testOuterJoins(Database &database)
    {
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l LEFT OUTER JOIN right_items AS r ON l.join_key = r.join_key;",
            {{1, null}, {2, 101}, {2, 102}, {3, 101}, {3, 102}, {4, null}, {5, null}});
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l RIGHT OUTER JOIN right_items AS r ON l.join_key = r.join_key;",
            {{2, 101}, {2, 102}, {3, 101}, {3, 102}, {null, 103}, {null, 104}});
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l FULL OUTER JOIN right_items AS r ON l.join_key = r.join_key;",
            {{1, null}, {2, 101}, {2, 102}, {3, 101}, {3, 102}, {4, null}, {5, null}, {null, 103}, {null, 104}});

        // Different input widths catch padding on the wrong side and shifted
        // column positions, including NULL values stored in actual input rows.
        expectRows(database,
            "SELECT * FROM left_items AS l FULL JOIN right_items AS r ON l.join_key = r.join_key;",
            {{1, 10, null, null, null},
             {2, 20, 101, 20, 7}, {2, 20, 102, 20, 8},
             {3, 20, 101, 20, 7}, {3, 20, 102, 20, 8},
             {4, 30, null, null, null}, {5, null, null, null, null},
             {null, null, 103, 40, 9}, {null, null, 104, null, 10}});
    }

    void testEmptyInputs(Database &database)
    {
        const Rows paddedLeft{
            {1, 10, null, null, null}, {2, 20, null, null, null},
            {3, 20, null, null, null}, {4, 30, null, null, null},
            {5, null, null, null, null}};
        const Rows paddedRight{
            {null, null, 101, 20, 7}, {null, null, 102, 20, 8},
            {null, null, 103, 40, 9}, {null, null, 104, null, 10}};
        struct JoinCase
        {
            std::string sql;
            bool preservesLeft;
            bool preservesRight;
        };
        for (const auto &join : std::vector<JoinCase>{
                 {"INNER JOIN", false, false}, {"CROSS JOIN", false, false},
                 {"LEFT JOIN", true, false}, {"RIGHT JOIN", false, true}, {"FULL JOIN", true, true}})
        {
            const std::string condition = join.sql == "CROSS JOIN"
                                              ? ";"
                                              : " ON l.join_key = r.join_key;";
            const auto withEmptyRight = expectRows(database,
                "SELECT * FROM left_items AS l " + join.sql + " empty_right AS r" + condition,
                join.preservesLeft ? paddedLeft : Rows{});
            const auto withEmptyLeft = expectRows(database,
                "SELECT * FROM empty_left AS l " + join.sql + " right_items AS r" + condition,
                join.preservesRight ? paddedRight : Rows{});
            const auto withBothEmpty = expectRows(database,
                "SELECT * FROM empty_left AS l " + join.sql + " empty_right AS r" + condition,
                {});
            require(withEmptyRight.columns.size() == 5 && withEmptyLeft.columns.size() == 5 &&
                        withBothEmpty.columns.size() == 5,
                    "Empty inputs must retain their schema widths for " + join.sql);
        }
    }

    void testDuplicateRows(Database &database)
    {
        expectRows(database,
            "SELECT l.id, r.id FROM duplicate_left AS l JOIN duplicate_right AS r ON l.join_key = r.join_key;",
            {{1, 2}, {1, 2}, {1, 2}, {1, 2}});
        expectRows(database,
            "SELECT l.id, r.id FROM duplicate_left AS l RIGHT JOIN duplicate_right AS r ON l.join_key = r.join_key;",
            {{1, 2}, {1, 2}, {1, 2}, {1, 2}, {null, 3}, {null, 3}});
        expectRows(database,
            "SELECT l.id, r.id FROM duplicate_left AS l FULL JOIN duplicate_right AS r ON l.join_key = r.join_key;",
            {{1, 2}, {1, 2}, {1, 2}, {1, 2}, {null, 3}, {null, 3}});
    }

    void testOnAndWhere(Database &database)
    {
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l JOIN right_items AS r ON FALSE;", {});
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l JOIN right_items AS r ON l.join_key = NULL;", {});
        for (const std::string condition : {"FALSE", "l.join_key = NULL"})
        {
            expectRows(database,
                "SELECT l.id, r.id FROM left_items AS l FULL JOIN right_items AS r ON " + condition + ";",
                {{1, null}, {2, null}, {3, null}, {4, null}, {5, null},
                 {null, 101}, {null, 102}, {null, 103}, {null, 104}});
        }
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l JOIN right_items AS r "
            "ON l.join_key IS NULL AND r.join_key IS NULL;", {{5, 104}});

        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l LEFT JOIN right_items AS r "
            "ON l.join_key = r.join_key AND r.payload > 7;",
            {{1, null}, {2, 102}, {3, 102}, {4, null}, {5, null}});
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l LEFT JOIN right_items AS r "
            "ON l.join_key = r.join_key WHERE r.payload > 7;", {{2, 102}, {3, 102}});
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l LEFT JOIN right_items AS r "
            "ON l.join_key = r.join_key WHERE r.id IS NULL;", {{1, null}, {4, null}, {5, null}});
        // Removing all matches with WHERE must not manufacture a NULL-padded row.
        expectRows(database,
            "SELECT l.id, r.id FROM left_items AS l LEFT JOIN right_items AS r "
            "ON l.join_key = r.join_key WHERE l.id = 2 AND r.payload = 999;", {});
        expectRows(database,
            "SELECT 100 / (r.payload - 7) FROM left_items AS l JOIN right_items AS r "
            "ON l.join_key = r.join_key WHERE r.payload > 7;", {{100}, {100}});
    }

    void testJoinAggregates(Database &database)
    {
        expectRows(database,
            "SELECT COUNT(*), COUNT(l.id), COUNT(r.id), SUM(l.id), SUM(r.payload) "
            "FROM left_items AS l FULL JOIN right_items AS r ON l.join_key = r.join_key;",
            {{std::int64_t{9}, std::int64_t{7}, std::int64_t{6}, std::int64_t{20}, std::int64_t{49}}});
        expectRows(database,
            "SELECT COUNT(*), COUNT(l.id), COUNT(r.id), SUM(r.payload) "
            "FROM left_items AS l FULL JOIN right_items AS r ON l.join_key = r.join_key WHERE r.payload >= 8;",
            {{std::int64_t{4}, std::int64_t{2}, std::int64_t{4}, std::int64_t{35}}});
        expectRows(database,
            "SELECT COUNT(*), COUNT(r.id), SUM(r.payload) "
            "FROM left_items AS l JOIN empty_right AS r ON l.join_key = r.join_key;",
            {{std::int64_t{0}, std::int64_t{0}, null}});
        expectRows(database,
            "SELECT COUNT(*), COUNT(r.id), SUM(r.payload) "
            "FROM left_items AS l LEFT JOIN empty_right AS r ON l.join_key = r.join_key;",
            {{std::int64_t{5}, std::int64_t{0}, null}});
        // WHERE must run before aggregate arguments (the excluded matches divide by zero).
        expectRows(database,
            "SELECT SUM(100 / (r.payload - 7)) FROM left_items AS l JOIN right_items AS r "
            "ON l.join_key = r.join_key WHERE r.payload > 7;", {{std::int64_t{200}}});
    }

    void testChainedJoins(Database &database)
    {
        expectRows(database,
            "SELECT l.id, r.id, a.tag FROM left_items AS l JOIN right_items AS r "
            "ON l.join_key = r.join_key JOIN annotations AS a ON r.id = a.right_id;",
            {{2, 101, std::string{"first"}}, {3, 101, std::string{"first"}}});
        expectRows(database,
            "SELECT l.id, r.id, a.tag FROM left_items AS l FULL JOIN right_items AS r "
            "ON l.join_key = r.join_key FULL JOIN annotations AS a ON r.id = a.right_id;",
            {{1, null, null}, {2, 101, std::string{"first"}}, {2, 102, null},
             {3, 101, std::string{"first"}}, {3, 102, null}, {4, null, null}, {5, null, null},
             {null, 103, std::string{"orphan"}}, {null, 104, null}, {null, null, std::string{"only"}}});
        // The first join produces no rows but still contributes five columns.
        const auto result = expectRows(database,
            "SELECT * FROM left_items AS l JOIN empty_right AS e ON l.join_key = e.join_key "
            "RIGHT JOIN right_items AS r ON e.id = r.id;",
            {{null, null, null, null, null, 101, 20, 7},
             {null, null, null, null, null, 102, 20, 8},
             {null, null, null, null, null, 103, 40, 9},
             {null, null, null, null, null, 104, null, 10}});
        require(result.columns.size() == 8, "An empty intermediate join lost its column count");
    }

    void testSelfJoins(Database &database)
    {
        database.executeSql("CREATE TABLE employees (id INT, manager_id INT, salary INT);");
        database.executeSql("INSERT INTO employees VALUES (1, NULL, 100), (2, 1, 20), (3, 1, 30), (4, 2, 40), (5, 99, 50);");
        expectRows(database,
            "SELECT e.id, m.id FROM employees AS e JOIN employees AS m ON e.manager_id = m.id;",
            {{2, 1}, {3, 1}, {4, 2}});
        expectRows(database,
            "SELECT e.id, m.id FROM employees AS e LEFT JOIN employees AS m ON e.manager_id = m.id;",
            {{1, null}, {2, 1}, {3, 1}, {4, 2}, {5, null}});
        expectRows(database,
            "SELECT e.id, m.id FROM employees AS e RIGHT JOIN employees AS m ON e.manager_id = m.id;",
            {{2, 1}, {3, 1}, {4, 2}, {null, 3}, {null, 4}, {null, 5}});
        expectRows(database,
            "SELECT e.id, m.id FROM employees AS e FULL JOIN employees AS m ON e.manager_id = m.id;",
            {{1, null}, {2, 1}, {3, 1}, {4, 2}, {5, null}, {null, 3}, {null, 4}, {null, 5}});
        expectRows(database,
            "SELECT m.*, e.* FROM employees AS e JOIN employees AS m ON e.manager_id = m.id WHERE e.id = 2;",
            {{1, null, 100, 2, 1, 20}});
        expectRows(database,
            "SELECT e.salary + m.salary, 1 AS marker, m.salary, e.salary FROM employees AS e "
            "JOIN employees AS m ON e.manager_id = m.id WHERE e.id = 2;",
            {{120, 1, 100, 20}});
        expectRows(database,
            "SELECT e.id, m.id, g.id FROM employees AS e LEFT JOIN employees AS m ON e.manager_id = m.id "
            "LEFT JOIN employees AS g ON m.manager_id = g.id;",
            {{1, null, null}, {2, 1, null}, {3, 1, null}, {4, 2, 1}, {5, null, null}});
        expectRows(database,
            "SELECT COUNT(*), SUM(m.salary) FROM employees AS e JOIN employees AS m ON e.manager_id = m.id;",
            {{std::int64_t{3}, std::int64_t{220}}});

        // Single-table mutations still bind against all physical columns.
        require(database.executeSql("UPDATE employees SET salary = salary + 1 WHERE id = 2;").affectedRows == 1,
                "UPDATE lost its binding context");
        expectRows(database, "SELECT salary FROM employees WHERE id = 2;", {{21}});
        require(database.executeSql("DELETE FROM employees AS e WHERE e.id = 5;").affectedRows == 1,
                "Aliased DELETE lost its binding context");
        expectRows(database, "SELECT id FROM employees;", {{1}, {2}, {3}, {4}});
    }
}

int main()
{
    TestDirectory directory{"db-join-executor-test"};
    auto database = Database::create(directory.path / "db", "db_join_executor_test");
    setup(database);

    using Test = std::pair<std::string, void (*)(Database &)>;
    const std::vector<Test> tests{
        {"inner and cross", testInnerAndCross},
        {"outer joins", testOuterJoins},
        {"empty inputs", testEmptyInputs},
        {"duplicate rows", testDuplicateRows},
        {"ON and WHERE", testOnAndWhere},
        {"aggregates", testJoinAggregates},
        {"chained joins", testChainedJoins},
        {"self joins", testSelfJoins}};
    int failures = 0;
    for (const auto &[name, test] : tests)
    {
        try
        {
            test(database);
        }
        catch (const std::exception &error)
        {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
