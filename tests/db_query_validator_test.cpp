#include "db_query_validator.h"

#include "db_decimal.h"
#include "db_expression_evaluator.h"
#include "db_page_factory.h"
#include "db_row_validator.h"
#include "db_write.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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
    void requireThrows(Function function, const std::string &expectedError)
    {
        try
        {
            function();
        }
        catch (const std::exception &error)
        {
            require(std::string{error.what()}.find(expectedError) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected error: " + expectedError);
    }

    // Exercise SQL binding and persisted schema metadata without depending on
    // the table cursor or buffer-manager writeback behavior.
    class MemoryCatalog final : public Catalog
    {
    public:
        bool tableExists(const std::string &name) const override
        {
            return tables.contains(name);
        }

        HeaderPage getTableHeader(const std::string &name) const override
        {
            return tables.at(name);
        }

        void add(const BoundCreateTable &table)
        {
            Page page = makeHeaderPage(table.tableName, "test", table.columns, table.constraints);
            Page decoded = decodeHeaderPage(encodePage(page));
            tables.insert_or_assign(table.tableName, std::get<HeaderPage>(std::move(decoded.data)));
        }

    private:
        std::map<std::string, HeaderPage> tables;
    };

    BoundQuery bind(QueryValidator &validator, const std::string &sql)
    {
        Lexer lexer{sql};
        Parser parser{lexer.tokenize()};
        return validator.validate(parser.parseStatement());
    }

    void create(MemoryCatalog &catalog, QueryValidator &validator, const std::string &sql)
    {
        catalog.add(std::get<BoundCreateTable>(bind(validator, sql)));
    }

    BoundInsert insert(QueryValidator &validator, const std::string &sql)
    {
        return std::get<BoundInsert>(bind(validator, sql));
    }

    void testDefaults(MemoryCatalog &catalog, QueryValidator &validator)
    {
        create(catalog, validator,
               "CREATE TABLE defaults (id INT NOT NULL, required INT NOT NULL DEFAULT 7, "
               "optional INT DEFAULT 9, label TEXT DEFAULT 'guest', enabled BOOLEAN DEFAULT TRUE, "
               "missing INT, empty_default INT DEFAULT NULL, big BIGINT DEFAULT 5, "
               "amount DECIMAL DEFAULT 12, approximate DOUBLE DEFAULT 0.25, "
               "computed INT DEFAULT 3 + 4, unknown_flag BOOLEAN DEFAULT 1 = NULL);");

        const HeaderPage schema = catalog.getTableHeader("defaults");
        BindContext context;
        context.insertTableName("defaults");
        context.insertCols(schema.columns, "defaults");
        context.insertConstraints(schema.constraints, "defaults");
        static_assert(std::is_same_v<
                      decltype(context.getDefaultConstraint(1, "defaults")),
                      const BoundDefaultConstraintExpr &>);
        require(!context.hasDefaultConstraint(0, "defaults") && context.findDefaultConstraint(0, "defaults") == nullptr,
                "An absent default must not be found");
        require(context.getDefaultConstraint(1, "defaults").columnId == 1,
                "Default lookup returned the wrong column");
        requireThrows([&] { context.getDefaultConstraint(0, "defaults"); }, "No default constraint");

        for (int repetition = 0; repetition < 2; ++repetition)
        {
            const BoundInsert result = insert(validator,
                "INSERT INTO defaults (optional, id, label) VALUES (NULL, 1, NULL), (4, 2, 'named');");
            require(result.rows.size() == 2, "INSERT batch has the wrong number of rows");
            for (const Row &row : result.rows)
            {
                const auto validation = validateRowAgainstSchema(schema.columns, row);
                require(validation.valid, "Default produced an invalid physical row: " + validation.message);
                require(std::get<std::int32_t>(row.values[1]) == 7,
                        "Omitted NOT NULL column did not receive its default");
                require(std::get<bool>(row.values[4]), "Boolean default was not applied");
                require(std::holds_alternative<std::monostate>(row.values[5]) &&
                            std::holds_alternative<std::monostate>(row.values[6]) &&
                            std::holds_alternative<std::monostate>(row.values[11]),
                        "Absent, explicit NULL, and computed NULL defaults are incorrect");
                require(std::get<std::int64_t>(row.values[7]) == 5,
                        "INT default was not converted to BIGINT");
                require(std::get<DecimalValue>(row.values[8]) == decimalFromInt64(12),
                        "INT default was not converted to DECIMAL");
                require(std::get<std::float64_t>(row.values[9]) == 0.25,
                        "DECIMAL default was not converted to DOUBLE");
                require(std::get<std::int32_t>(row.values[10]) == 7,
                        "Arithmetic default was not applied");
            }
            require(std::get<std::int32_t>(result.rows[0].values[0]) == 1 &&
                        std::holds_alternative<std::monostate>(result.rows[0].values[2]) &&
                        std::holds_alternative<std::monostate>(result.rows[0].values[3]),
                    "Explicit NULL was replaced with a default or columns were reordered incorrectly");
            require(std::get<std::int32_t>(result.rows[1].values[2]) == 4 &&
                        std::get<std::string>(result.rows[1].values[3]) == "named",
                    "Explicit values were overwritten");
        }

        const BoundInsert omitted = insert(validator, "INSERT INTO defaults (id) VALUES (3);");
        require(std::get<std::int32_t>(omitted.rows[0].values[2]) == 9 &&
                    std::get<std::string>(omitted.rows[0].values[3]) == "guest",
                "Nullable columns must also receive omitted defaults");
        requireThrows([&] { insert(validator, "INSERT INTO defaults (id, required) VALUES (1, NULL);"); },
                      "NOT NULL");
        requireThrows([&] { insert(validator, "INSERT INTO defaults (optional) VALUES (1);"); },
                      "NOT NULL");
        requireThrows([&] { insert(validator, "INSERT INTO defaults (id, id) VALUES (1, 2);"); },
                      "Duplicate INSERT column");
        requireThrows([&] { insert(validator, "INSERT INTO defaults (id) VALUES (1, 2);"); },
                      "column count");
    }

    void testDefaultErrors(MemoryCatalog &catalog, QueryValidator &validator)
    {
        create(catalog, validator,
               "CREATE TABLE null_default (id INT, value INT NOT NULL DEFAULT NULL);");
        require(insert(validator, "INSERT INTO null_default VALUES (1, 3);").rows.size() == 1,
                "A NULL default must not prevent providing a valid explicit value");
        requireThrows([&] { insert(validator, "INSERT INTO null_default (id) VALUES (1);"); },
                      "NOT NULL");
        requireThrows([&] { insert(validator, "INSERT INTO null_default VALUES (1, NULL);"); },
                      "NOT NULL");

        create(catalog, validator,
               "CREATE TABLE computed_null (id INT, value BOOLEAN NOT NULL DEFAULT 1 = NULL);");
        requireThrows([&] { insert(validator, "INSERT INTO computed_null (id) VALUES (1);"); },
                      "NOT NULL");

        auto failingTable = std::get<BoundCreateTable>(bind(validator,
            "CREATE TABLE failing_default (id INT, value INT DEFAULT 0);"));
        // Install an unfolded bound expression: the binder normally folds
        // literal arithmetic, but stored expressions must also be evaluated
        // lazily, only when an INSERT actually needs the default.
        std::get<BoundDefaultConstraintExpr>(failingTable.constraints[0]).value =
            std::make_unique<BoundBinaryExpr>(
                BinaryOperator::Divide,
                std::make_unique<BoundLiteralExpr>(std::int32_t{1}, DataType::Int),
                std::make_unique<BoundLiteralExpr>(std::int32_t{0}, DataType::Int),
                DataType::Int);
        catalog.add(failingTable);
        requireThrows([&] { insert(validator, "INSERT INTO failing_default (id) VALUES (1);"); },
                      "Division by zero");
        const BoundInsert supplied = insert(validator,
            "INSERT INTO failing_default VALUES (1, 5), (2, NULL);");
        require(std::get<std::int32_t>(supplied.rows[0].values[1]) == 5 &&
                    std::holds_alternative<std::monostate>(supplied.rows[1].values[1]),
                "Unused defaults must not be evaluated");

        for (const std::string sql : {
                 "CREATE TABLE bad (id INT, value INT DEFAULT id);",
                 "CREATE TABLE bad (id INT, value INT DEFAULT ABS(id));",
                 "CREATE TABLE bad (id INT, value BOOLEAN DEFAULT id IS NULL);"})
        {
            requireThrows([&] { bind(validator, sql); }, "cannot reference table columns");
        }
        requireThrows([&] { bind(validator, "CREATE TABLE bad (id INT DEFAULT 1 DEFAULT 2);"); },
                      "Multiple DEFAULT");
        requireThrows([&] { bind(validator, "CREATE TABLE bad (value TEXT DEFAULT 1);"); },
                      "incompatible");
        requireThrows([&] { bind(validator, "CREATE TABLE bad (id INT DEFAULT COUNT(*));"); },
                      "Aggregate functions are not allowed");
    }

    void testLiteralConversions(MemoryCatalog &catalog, QueryValidator &validator)
    {
        create(catalog, validator,
               "CREATE TABLE numbers (small INT, big BIGINT, amount DECIMAL, approximate DOUBLE, "
               "label TEXT, enabled BOOLEAN);");
        const BoundInsert result = insert(validator,
            "INSERT INTO numbers VALUES "
            "(-2147483648, 9223372036854775807, -12.3400, 0.1, 'hello', FALSE), "
            "(7.000, -2147483649, +5, -2, NULL, TRUE);");
        const auto schema = catalog.getTableHeader("numbers");
        for (const Row &row : result.rows)
        {
            require(validateRowAgainstSchema(schema.columns, row).valid,
                    "Literal conversion produced the wrong physical types");
        }
        const auto &first = result.rows[0].values;
        require(std::get<std::int32_t>(first[0]) == std::numeric_limits<std::int32_t>::min() &&
                    std::get<std::int64_t>(first[1]) == std::numeric_limits<std::int64_t>::max(),
                "Integer boundary values lost range or precision");
        require(std::get<DecimalValue>(first[2]) == parseDecimalLiteral("-12.34") &&
                    std::abs(std::get<std::float64_t>(first[3]) - 0.1) < 1e-12,
                "Decimal or floating-point literal conversion is incorrect");
        require(std::get<std::int32_t>(result.rows[1].values[0]) == 7 &&
                    std::get<DecimalValue>(result.rows[1].values[2]) == decimalFromInt64(5),
                "Exact cross-numeric literal conversion failed");

        for (const std::string value : {"2147483648", "-2147483649"})
        {
            requireThrows([&] { insert(validator, "INSERT INTO numbers (small) VALUES (" + value + ");"); },
                          "out of range");
        }
        for (const std::string column : {"small", "big"})
        {
            requireThrows([&] { insert(validator, "INSERT INTO numbers (" + column + ") VALUES (1.5);"); },
                          "integer numeric value");
        }
        requireThrows([&] { insert(validator, "INSERT INTO numbers (amount) VALUES (0.1234567890123456789);"); },
                      "Decimal scale");
        for (const std::string sql : {
                 "INSERT INTO numbers (small) VALUES ('1');",
                 "INSERT INTO numbers (small) VALUES (TRUE);",
                 "INSERT INTO numbers (label) VALUES (1);",
                 "INSERT INTO numbers (enabled) VALUES (1);"})
        {
            requireThrows([&] { insert(validator, sql); }, "Expected");
        }

        // Preserve literal-only INSERT semantics, even when generic binding
        // could fold an expression into a literal or evaluate a function.
        requireThrows([&] { insert(validator, "INSERT INTO numbers (small) VALUES (ABS(-1));"); },
                      "Function calls are not allowed");
        for (const std::string expression : {"-ABS(1)", "1 + 2", "small"})
        {
            requireThrows([&] { insert(validator, "INSERT INTO numbers (small) VALUES (" + expression + ");"); },
                          "INSERT values must be literals");
        }
        // Other non-literal forms must remain rejected, whether during parsing
        // or binding.
        for (const std::string expression : {"NOT TRUE", "1 IS NULL"})
        {
            requireThrows([&] { insert(validator, "INSERT INTO numbers (small) VALUES (" + expression + ");"); },
                          "");
        }
    }

    void testFloatingPointInputs(QueryValidator &validator)
    {
        const auto bindNumber = [&](const std::string &column, NumberValue number)
        {
            std::vector<std::unique_ptr<Expr>> values;
            values.push_back(std::make_unique<NumberExpr>(std::move(number)));
            std::vector<std::vector<std::unique_ptr<Expr>>> rows;
            rows.push_back(std::move(values));
            const Statement statement = InsertStatement{"numbers", {column}, std::move(rows)};
            return std::get<BoundInsert>(validator.validate(statement));
        };
        require(std::get<std::int64_t>(bindNumber("big", std::float64_t{42}).rows[0].values[1]) == 42,
                "Integral floating-point input was not converted exactly");
        require(std::get<std::int64_t>(bindNumber("big", -std::ldexp(1.0, 63)).rows[0].values[1]) ==
                    std::numeric_limits<std::int64_t>::min(),
                "INT64_MIN must be accepted without an overflowing cast");
        requireThrows([&] { bindNumber("big", std::ldexp(1.0, 63)); }, "out of range");
        requireThrows([&] { bindNumber("big", std::float64_t{1.5}); }, "integer numeric value");
        requireThrows([&] { bindNumber("big", std::numeric_limits<std::float64_t>::infinity()); },
                      "out of range");
        requireThrows([&] { bindNumber("big", std::numeric_limits<std::float64_t>::quiet_NaN()); },
                      "out of range");
        requireThrows([&] { bindNumber("approximate", std::numeric_limits<std::float64_t>::infinity()); },
                      "out of range");
        require(std::get<std::float64_t>(bindNumber("approximate", static_cast<std::float32_t>(0.5)).rows[0].values[3]) == 0.5,
                "FLOAT input was not widened to DOUBLE");
        requireThrows([&] { bindNumber("amount", std::float64_t{1.5}); }, "Expected an integer value");
    }

    void testTableReferences(MemoryCatalog &catalog, QueryValidator &validator)
    {
        create(catalog, validator, "CREATE TABLE authors (id INT PRIMARY KEY, author_name TEXT);");
        create(catalog, validator, "CREATE TABLE books (id INT, author_id INT, title TEXT);");
        create(catalog, validator, "CREATE TABLE ratings (book_id INT, score INT);");

        auto select = std::get<BoundSelect>(bind(validator,
            "SELECT a.author_name, b.title FROM authors AS a "
            "JOIN books AS b ON a.id = b.author_id WHERE b.id = 10;"));
        const auto &join = std::get<BoundJoinTableRef>(select.from->value);
        require(join.type == JoinType::Inner && join.condition,
                "JOIN type or ON condition was lost");
        require(std::get<BoundNamedTableRef>(join.left->value).alias == "a" &&
                    std::get<BoundNamedTableRef>(join.right->value).alias == "b",
                "Table aliases were lost during binding");
        const Row joinedRow{.values = {
            std::int32_t{7}, std::string{"Ada"},
            std::int32_t{10}, std::int32_t{7}, std::string{"Book"}}};
        require(evaluatePredicate(*join.condition, joinedRow) &&
                    evaluatePredicate(*select.where, joinedRow),
                "JOIN or WHERE bound a right-hand column to its local table position");
        require(std::get<std::string>(evaluateValue(*select.projections[0].expr, joinedRow)) == "Ada" &&
                    std::get<std::string>(evaluateValue(*select.projections[1].expr, joinedRow)) == "Book",
                "Qualified projections resolved the wrong columns");

        select = std::get<BoundSelect>(bind(validator,
            "SELECT *, b.*, author_name, title FROM authors AS a "
            "CROSS JOIN books AS b;"));
        require(!std::get<BoundJoinTableRef>(select.from->value).condition,
                "CROSS JOIN should have no ON condition");
        require(select.projections.size() == 10, "Wildcard expanded the wrong tables");
        for (std::size_t i = 0; i < joinedRow.values.size(); ++i)
            require(evaluateValue(*select.projections[i].expr, joinedRow) == joinedRow.values[i],
                    "Wildcard columns must follow left-to-right table order");
        for (std::size_t i = 0; i < 3; ++i)
            require(evaluateValue(*select.projections[i + 5].expr, joinedRow) == joinedRow.values[i + 2],
                    "Qualified wildcard must select only its table's columns");
        require(evaluateValue(*select.projections[8].expr, joinedRow) == joinedRow.values[1] &&
                    evaluateValue(*select.projections[9].expr, joinedRow) == joinedRow.values[4],
                "Unqualified columns must be searched in every table");

        select = std::get<BoundSelect>(bind(validator,
            "SELECT r.score FROM authors AS a JOIN books AS b ON a.id = b.author_id "
            "LEFT JOIN ratings AS r ON b.id = r.book_id;"));
        const auto &outerJoin = std::get<BoundJoinTableRef>(select.from->value);
        require(outerJoin.type == JoinType::LeftOuter &&
                    std::holds_alternative<BoundJoinTableRef>(outerJoin.left->value),
                "Chained JOINs lost their left subtree");
        Row ratedRow = joinedRow;
        ratedRow.values.push_back(std::int32_t{10});
        ratedRow.values.push_back(std::int32_t{5});
        require(evaluatePredicate(*outerJoin.condition, ratedRow) &&
                    std::get<std::int32_t>(evaluateValue(*select.projections[0].expr, ratedRow)) == 5,
                "Chained JOIN column positions are incorrect");

        requireThrows([&] { bind(validator, "SELECT id FROM authors CROSS JOIN books;"); },
                      "Ambiguous column");
        requireThrows([&] { bind(validator, "SELECT missing FROM authors CROSS JOIN books;"); },
                      "Unknown column");
        requireThrows([&] { bind(validator, "SELECT * FROM authors AS x CROSS JOIN books AS x;"); },
                      "Duplicate table alias");
        requireThrows([&] { bind(validator, "SELECT * FROM missing;"); }, "Table does not exist");
        requireThrows([&] { bind(validator, "SELECT 1;"); }, "SELECT requires a FROM clause");
        requireThrows([&] { bind(validator, "DELETE FROM authors CROSS JOIN books;"); },
                      "Only single-table DELETE");

        // Reuse the validator after both successful joins and binding errors.
        select = std::get<BoundSelect>(bind(validator, "SELECT id FROM authors;"));
        require(std::get<std::int32_t>(evaluateValue(*select.projections[0].expr, joinedRow)) == 7,
                "A previous statement leaked its tables or column positions");
        auto del = std::get<BoundDelete>(bind(validator, "DELETE FROM authors AS a WHERE a.id = 7;"));
        require(std::get<BoundNamedTableRef>(del.from->value).tableName == "authors" &&
                    evaluatePredicate(*del.where, joinedRow),
                "DELETE lost its target or failed to resolve its alias");
    }
}

int main()
{
    MemoryCatalog catalog;
    QueryValidator validator{catalog};
    testDefaults(catalog, validator);
    testDefaultErrors(catalog, validator);
    testLiteralConversions(catalog, validator);
    testFloatingPointInputs(validator);
    testTableReferences(catalog, validator);
}
