#include "db_database.h"
#include "db_decimal.h"

#include <filesystem>
#include <stdexcept>
#include <string>
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
}

int main()
{
    const DecimalValue first = parseDecimalLiteral("12.3400");
    const DecimalValue second = parseDecimalLiteral("0.66");
    require(
        first == DecimalValue{.coefficient = 1234, .scale = 2},
        "Decimal parsing did not preserve the exact value");
    require(
        decimalToString(addDecimals(first, second)) == "13",
        "Exact decimal addition failed");
    require(
        parseDecimalLiteral("1.0") == parseDecimalLiteral("1.00"),
        "Equivalent decimal scales compared unequal");
    require(
        decimalToString(divideDecimals(
            parseDecimalLiteral("1.0"),
            parseDecimalLiteral("8.0"))) == "0.125",
        "Exact decimal division failed");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "db_decimal_test_data";
    std::filesystem::remove_all(path);

    Database database{path, "db_decimal_test"};
    database.executeSql(
        "CREATE TABLE amounts ("
        "id INT, amount DECIMAL, approximate DOUBLE, large BIGINT);");
    database.executeSql(
        "INSERT INTO amounts VALUES "
        "(1, 12.3400, 0.1, 2147483648);");
    database.executeSql(
        "INSERT INTO amounts VALUES "
        "(2, -0.25, -1.5, -2147483649);");

    QueryResult result = database.executeSql("SELECT * FROM amounts;");
    require(result.rows.size() == 2, "Stored row count is incorrect");
    require(
        std::get<DecimalValue>(result.rows[0].values[1]) == first,
        "DECIMAL did not survive its disk round-trip");
    require(
        std::holds_alternative<std::float64_t>(result.rows[0].values[2]),
        "DOUBLE conversion produced the wrong C++ type");
    require(
        std::get<std::int64_t>(result.rows[0].values[3]) == 2147483648LL,
        "BIGINT did not survive its disk round-trip");

    result = database.executeSql(
        "SELECT amount FROM amounts WHERE amount = 12.34;");
    require(result.rows.size() == 1, "Exact decimal equality failed");

    result = database.executeSql(
        "SELECT amount FROM amounts WHERE amount = 12 + 0.34;");
    require(result.rows.size() == 1, "Exact decimal constant folding failed");

    result = database.executeSql(
        "SELECT amount FROM amounts WHERE amount > 12;");
    require(result.rows.size() == 1, "DECIMAL/INT comparison failed");

    result = database.executeSql(
        "SELECT amount FROM amounts WHERE amount < 0;");
    require(result.rows.size() == 1, "Negative decimal comparison failed");

    result = database.executeSql(
        "SELECT approximate FROM amounts WHERE approximate = 0.1;");
    require(
        result.rows.size() == 1,
        "Contextual DECIMAL-to-DOUBLE comparison failed");

    bool rejectedExcessScale = false;
    try
    {
        database.executeSql(
            "INSERT INTO amounts VALUES "
            "(3, 0.1234567890123456789, 0.0, 3);");
    }
    catch (const std::overflow_error &)
    {
        rejectedExcessScale = true;
    }
    require(rejectedExcessScale, "Excess decimal scale was not rejected");

    std::filesystem::remove_all(path);
}
