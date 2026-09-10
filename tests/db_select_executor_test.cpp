#include "db_database.h"
#include "db_decimal.h"
#include "db_query_executor.h"
#include "db_table.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

namespace
{
    void require(bool condition, const std::string &message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename Function>
    void requireThrows(
        Function function,
        const std::string &message,
        const std::string &expectedError = {})
    {
        try
        {
            function();
        }
        catch (const std::runtime_error &error)
        {
            require(std::string{error.what()}.find(expectedError) != std::string::npos,
                    message + ": unexpected error: " + error.what());
            return;
        }

        throw std::runtime_error(message);
    }

    void testAggregates(Database &database)
    {
        QueryResult result = database.executeSql(
            "SELECT SUM(price * quantity) AS revenue, COUNT(*), COUNT(price - 5), "
            "SUM(1), 7, ABS(SUM(price - 12)), -SUM(quantity), "
            "SUM(price) + COUNT(*), SUM(price) IS NULL "
            "FROM products WHERE id >= 11;");
        require(result.returnsRows && result.affectedRows == 0,
                "Aggregate SELECT has incorrect result metadata");
        require(result.rows.size() == 1 && result.rows[0].values.size() == 9,
                "Aggregates did not produce exactly one complete row");
        require(result.columns.size() == 9 && result.columns[0].name == "revenue" &&
                    result.columns[1].name == "COUNT(*)",
                "Aggregate output names are incorrect");
        require(result.columns[0].type == DataType::BigInt &&
                    result.columns[1].type == DataType::BigInt,
                "Integer SUM and COUNT must return BIGINT");
        const auto &values = result.rows[0].values;
        require(std::get<std::int64_t>(values[0]) == 88,
                "SUM evaluated the wrong physical columns or ignored WHERE");
        require(std::get<std::int64_t>(values[1]) == 2 &&
                    std::get<std::int64_t>(values[2]) == 2 &&
                    std::get<std::int64_t>(values[3]) == 2,
                "COUNT or constant SUM ignored WHERE");
        require(std::get<std::int32_t>(values[4]) == 7,
                "Constant projection alongside aggregates failed");
        require(std::get<std::int64_t>(values[5]) == 8 &&
                    std::get<std::int64_t>(values[6]) == -6 &&
                    std::get<std::int64_t>(values[7]) == 34 &&
                    !std::get<bool>(values[8]),
                "Expression around an aggregate was evaluated incorrectly");

        result = database.executeSql(
            "SELECT SUM(12 / ABS(id - 10)) FROM products WHERE id > 10;");
        require(std::get<std::int64_t>(result.rows[0].values[0]) == 18,
                "WHERE must run before aggregate arguments are evaluated");

        result = database.executeSql(
            "SELECT COUNT(*), COUNT(price), SUM(price), SUM(1), 7, "
            "SUM(price) IS NULL FROM products WHERE id < 0;");
        require(result.rows.size() == 1 && result.rows[0].values.size() == 6,
                "An aggregate over no matching rows must still return one row");
        require(std::get<std::int64_t>(result.rows[0].values[0]) == 0 &&
                    std::get<std::int64_t>(result.rows[0].values[1]) == 0 &&
                    std::holds_alternative<std::monostate>(result.rows[0].values[2]) &&
                    std::holds_alternative<std::monostate>(result.rows[0].values[3]) &&
                    std::get<std::int32_t>(result.rows[0].values[4]) == 7 &&
                    std::get<bool>(result.rows[0].values[5]),
                "Empty aggregate results are incorrect");

        database.executeSql(
            "CREATE TABLE aggregate_values (id INT, small INT, large BIGINT, "
            "approximate DOUBLE, amount DECIMAL, label TEXT, enabled BOOLEAN);");
        result = database.executeSql(
            "SELECT COUNT(*), SUM(small) FROM aggregate_values;");
        require(result.rows.size() == 1 &&
                    std::get<std::int64_t>(result.rows[0].values[0]) == 0 &&
                    std::holds_alternative<std::monostate>(result.rows[0].values[1]),
                "Aggregates over a physically empty table are incorrect");
        database.executeSql(
            "INSERT INTO aggregate_values VALUES "
            "(1, 2147483647, 2147483648, 0.1, 12.3400, 'first', TRUE), "
            "(2, 10, -2147483649, -1.5, -0.25, NULL, FALSE), "
            "(3, NULL, NULL, NULL, NULL, 'third', NULL), "
            "(4, -5, 10, 0.4, 0.66, 'fourth', TRUE);");
        result = database.executeSql(
            "SELECT sum(small), SUM(large), SUM(approximate), SUM(amount), "
            "count(*), COUNT(small), COUNT(label), COUNT(enabled) "
            "FROM aggregate_values;");
        require(result.rows.size() == 1 && result.rows[0].values.size() == 8,
                "Multiple aggregates produced an incorrect result shape");
        require(std::get<std::int64_t>(result.rows[0].values[0]) == 2147483652LL,
                "SUM(INT) did not widen its accumulator to BIGINT");
        require(std::get<std::int64_t>(result.rows[0].values[1]) == 9,
                "SUM(BIGINT) is incorrect");
        require(result.columns[2].type == DataType::Double &&
                    std::abs(std::get<std::float64_t>(result.rows[0].values[2]) + 1.0) < 1e-12,
                "SUM(DOUBLE) is incorrect");
        require(result.columns[3].type == DataType::Decimal &&
                    std::get<DecimalValue>(result.rows[0].values[3]) ==
                        parseDecimalLiteral("12.75"),
                "SUM(DECIMAL) lost sign or scale");
        require(std::get<std::int64_t>(result.rows[0].values[4]) == 4 &&
                    std::get<std::int64_t>(result.rows[0].values[5]) == 3 &&
                    std::get<std::int64_t>(result.rows[0].values[6]) == 3 &&
                    std::get<std::int64_t>(result.rows[0].values[7]) == 3,
                "COUNT must count all non-null values, including FALSE");

        result = database.executeSql(
            "SELECT COUNT(*), COUNT(small), SUM(small), COUNT(NULL), SUM(NULL) "
            "FROM aggregate_values WHERE id = 3;");
        require(std::get<std::int64_t>(result.rows[0].values[0]) == 1 &&
                    std::get<std::int64_t>(result.rows[0].values[1]) == 0 &&
                    std::holds_alternative<std::monostate>(result.rows[0].values[2]) &&
                    std::get<std::int64_t>(result.rows[0].values[3]) == 0 &&
                    std::holds_alternative<std::monostate>(result.rows[0].values[4]),
                "Aggregates over all-null inputs are incorrect");
        result = database.executeSql(
            "SELECT SUM(small) FROM aggregate_values WHERE id >= 3;");
        require(std::get<std::int64_t>(result.rows[0].values[0]) == -5,
                "SUM did not handle leading NULL inputs");

        for (const std::string sql : {
                 "SELECT id, SUM(price) FROM products;",
                 "SELECT SUM(price), id FROM products;",
                 "SELECT SUM(price) + id FROM products;",
                 "SELECT id + COUNT(*) FROM products WHERE id < 0;",
                 "SELECT ABS(id), COUNT(*) FROM products;"})
        {
            requireThrows([&database, &sql] { database.executeSql(sql); },
                          "Ungrouped column was not rejected: " + sql, "GROUP BY");
        }
        for (const std::string sql : {
                 "SELECT SUM(COUNT(*)) FROM products;",
                 "SELECT COUNT(ABS(SUM(price))) FROM products;",
                 "SELECT id FROM products WHERE COUNT(*) > 0;",
                 "SELECT id FROM products WHERE SUM(price) > 0 AND id < 0;",
                 "DELETE FROM products WHERE COUNT(*) > 0;",
                 "CREATE TABLE invalid_default (id INT DEFAULT COUNT(*));",
                 "CREATE TABLE invalid_check (id INT, CHECK (SUM(id) > 0));"})
        {
            requireThrows([&database, &sql] { database.executeSql(sql); },
                          "Invalid aggregate context was accepted: " + sql,
                          "Aggregate functions are not allowed");
        }
        for (const std::string sql : {
                 "SELECT COUNT() FROM products;",
                 "SELECT COUNT(id, price) FROM products;",
                 "SELECT SUM() FROM products;",
                 "SELECT SUM(id, price) FROM products;",
                 "SELECT SUM(label) FROM aggregate_values;",
                 "SELECT SUM(enabled) FROM aggregate_values;"})
        {
            requireThrows([&database, &sql] { database.executeSql(sql); },
                          "Invalid aggregate argument was accepted: " + sql, "requires");
        }
        requireThrows(
            [&database] { database.executeSql("SELECT SUM(*) FROM products;"); },
            "SUM(*) was accepted", "Only COUNT");
        for (const std::string name : {"AVG", "MIN", "MAX"})
        {
            requireThrows(
                [&database, &name]
                {
                    database.executeSql("SELECT " + name + "(price) FROM products;");
                },
                "Unsupported aggregate was accepted: " + name, "not supported yet");
        }

        database.executeSql("CREATE TABLE overflow_values (number BIGINT);");
        database.insertRows("overflow_values", {
            Row{{std::numeric_limits<std::int64_t>::max()}}, Row{{std::int64_t{1}}}});
        requireThrows(
            [&database] { database.executeSql("SELECT SUM(number) FROM overflow_values;"); },
            "Positive SUM overflow was not detected", "overflow");
        database.executeSql("DELETE FROM overflow_values;");
        database.insertRows("overflow_values", {
            Row{{std::numeric_limits<std::int64_t>::min()}}, Row{{std::int64_t{-1}}}});
        requireThrows(
            [&database] { database.executeSql("SELECT SUM(number) FROM overflow_values;"); },
            "Negative SUM overflow was not detected", "overflow");

        database.executeSql("CREATE TABLE decimal_overflow (number DECIMAL);");
        database.insertRows("decimal_overflow", {
            Row{{DecimalValue{std::numeric_limits<std::int64_t>::max(), 0}}},
            Row{{DecimalValue{1, 0}}}});
        requireThrows(
            [&database] { database.executeSql("SELECT SUM(number) FROM decimal_overflow;"); },
            "Decimal SUM overflow was not detected", "overflow");

        database.executeSql("CREATE TABLE many_values (number INT);");
        std::vector<Row> rows;
        for (std::int32_t number = 1; number <= 1500; ++number)
        {
            rows.push_back(Row{{number}});
        }
        database.insertRows("many_values", rows);
        result = database.executeSql("SELECT COUNT(*), SUM(number) FROM many_values;");
        require(std::get<std::int64_t>(result.rows[0].values[0]) == 1500 &&
                    std::get<std::int64_t>(result.rows[0].values[1]) == 1125750,
                "Aggregates did not traverse all data pages");
        database.executeSql("DELETE FROM many_values WHERE number <= 1000;");
        result = database.executeSql("SELECT COUNT(*), SUM(number) FROM many_values;");
        require(std::get<std::int64_t>(result.rows[0].values[0]) == 500 &&
                    std::get<std::int64_t>(result.rows[0].values[1]) == 625250,
                "Aggregates included deleted rows or stopped at an empty page");
    }
}

int main()
{
    static_assert(!std::is_same_v<Row, ResultRow>);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "db_select_executor_test_data";
    std::filesystem::remove_all(path);

    Database database{path, "db_select_executor_test"};
    QueryResult create = database.executeSql(
        "CREATE TABLE products (id INT, price INT, quantity INT);");
    require(!create.returnsRows, "CREATE unexpectedly returned rows");
    require(create.columns.empty(), "CREATE returned an output schema");

    QueryResult insert = database.executeSql(
        "INSERT INTO products VALUES (10, 5, 3), (11, 12, 4), (12, 20, 2);");
    require(insert.affectedRows == 3, "INSERT affected-row count is incorrect");
    require(insert.columns.empty(), "INSERT returned an output schema");

    QueryResult result = database.executeSql(
        "SELECT 1 AS test, id, price * quantity AS total, "
        "ABS(price - 12) AS distance FROM products WHERE id >= 11;");

    require(result.returnsRows, "SELECT did not identify itself as row-returning");
    require(result.affectedRows == 0, "SELECT reported affected rows");
    require(result.columns.size() == 4, "Projection schema has the wrong size");
    require(result.columns[0].name == "test", "Literal alias was not preserved");
    require(result.columns[0].type == DataType::Int, "Literal type is incorrect");
    require(result.columns[1].name == "id", "Column output name is incorrect");
    require(result.columns[1].type == DataType::Int, "Column type is incorrect");
    require(result.columns[2].name == "total", "Expression alias was not preserved");
    require(result.columns[2].type == DataType::Int, "Expression type is incorrect");
    require(result.columns[3].name == "distance", "Function alias was not preserved");
    require(result.columns[3].type == DataType::Int, "Function type is incorrect");

    require(result.rows.size() == 2, "WHERE produced the wrong result-row count");
    require(std::get<std::int32_t>(result.rows[0].values[0]) == 1,
            "Literal projection is incorrect");
    require(std::get<std::int32_t>(result.rows[0].values[1]) == 11,
            "Column projection is incorrect");
    require(std::get<std::int32_t>(result.rows[0].values[2]) == 48,
            "Arithmetic projection is incorrect");
    require(std::get<std::int32_t>(result.rows[0].values[3]) == 0,
            "Scalar-function projection is incorrect");
    require(std::get<std::int32_t>(result.rows[1].values[2]) == 40,
            "Second arithmetic projection is incorrect");
    require(std::get<std::int32_t>(result.rows[1].values[3]) == 8,
            "Second scalar-function projection is incorrect");

    result = database.executeSql(
        "SELECT products.id, 1, price * quantity, -price FROM products WHERE id = 10;");
    require(result.columns[0].name == "id", "Qualified column was not unqualified");
    require(result.columns[1].name == "1", "Literal output name is incorrect");
    require(result.columns[2].name == "price * quantity",
            "Arithmetic output name is incorrect");
    require(result.columns[3].name == "-price", "Unary output name is incorrect");
    require(std::get<std::int32_t>(result.rows[0].values[3]) == -5,
            "Unary projection is incorrect");

    result = database.executeSql("SELECT products.* FROM products;");
    require(result.columns.size() == 3, "Wildcard schema has the wrong size");
    require(result.columns[0].name == "id" &&
            result.columns[1].name == "price" &&
            result.columns[2].name == "quantity",
            "Wildcard schema has the wrong names");
    require(result.rows.size() == 3, "Wildcard scan has the wrong row count");

    const std::vector<Row> physicalRows = database.selectAllRows("products");
    require(physicalRows.size() == 3, "Physical-row compatibility scan failed");

    testAggregates(database);

    requireThrows(
        [&database]
        {
            database.executeSql("SELECT id implicit_alias FROM products;");
        },
        "Implicit alias was not rejected");

    Table table = Table::open(path / "tables" / "products.table");
    BoundSelect grouped{
        .tableName = "products",
        .projections = {},
        .where = nullptr,
        .groupBy = {},
        .having = nullptr};
    grouped.projections.push_back(BoundSelectItem{
        .expr = std::make_unique<BoundColumnExpr>(0, DataType::Int),
        .outputName = "id"});
    grouped.groupBy.push_back(
        std::make_unique<BoundColumnExpr>(0, DataType::Int));

    SelectExecutor executor;
    BoundSelect floatingSum{
        .tableName = "products",
        .projections = {},
        .where = nullptr,
        .groupBy = {},
        .having = nullptr};
    std::vector<std::unique_ptr<BoundExpr>> arguments;
    arguments.push_back(std::make_unique<BoundLiteralExpr>(
        Value{static_cast<std::float32_t>(0.5)}, DataType::Float));
    floatingSum.projections.push_back(BoundSelectItem{
        .expr = std::make_unique<BoundFunctionCall>(
            FunctionId::Sum, FunctionCategory::Aggregate,
            std::move(arguments), DataType::Double),
        .outputName = "total"});
    for (int execution = 0; execution < 2; ++execution)
    {
        result = executor.execute(floatingSum, table);
        require(result.rows.size() == 1 &&
                    std::get<std::float64_t>(result.rows[0].values[0]) == 1.5,
                "FLOAT accumulation or repeated bound-query execution failed");
    }

    requireThrows(
        [&executor, &grouped, &table]
        {
            executor.execute(grouped, table);
        },
        "A manually populated GROUP BY was not rejected");

    QueryResult deleted = database.executeSql(
        "DELETE FROM products WHERE id = 12;");
    require(!deleted.returnsRows, "DELETE unexpectedly returned rows");
    require(deleted.affectedRows == 1, "DELETE affected-row count is incorrect");
    require(deleted.columns.empty(), "DELETE returned an output schema");
    require(deleted.rows.empty(), "DELETE returned result rows");

    std::filesystem::remove_all(path);
}
