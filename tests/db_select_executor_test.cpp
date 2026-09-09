#include "db_database.h"
#include "db_query_executor.h"
#include "db_table.h"

#include <filesystem>
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
    void requireThrows(Function function, const std::string &message)
    {
        try
        {
            function();
        }
        catch (const std::runtime_error &)
        {
            return;
        }

        throw std::runtime_error(message);
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

    requireThrows(
        [&database]
        {
            database.executeSql("SELECT COUNT(*) AS count FROM products;");
        },
        "Aggregate projection was not rejected");

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
