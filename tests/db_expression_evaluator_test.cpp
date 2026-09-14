#include "db_expression_evaluator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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

    template <typename Function>
    void requireThrows(Function function, const std::string &expectedError)
    {
        try
        {
            function();
        }
        catch (const std::runtime_error &error)
        {
            require(std::string{error.what()}.find(expectedError) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected error: " + expectedError);
    }

    void requireTruth(const Value &value, SqlTruth expected, const std::string &message)
    {
        if (expected == SqlTruth::Unknown)
        {
            require(std::holds_alternative<std::monostate>(value), message);
        }
        else
        {
            const auto *boolean = std::get_if<bool>(&value);
            require(boolean && *boolean == (expected == SqlTruth::True), message);
        }
    }

    BoundBinaryExpr binaryColumns(
        BinaryOperator op,
        DataType leftType = DataType::Boolean,
        DataType rightType = DataType::Boolean)
    {
        return BoundBinaryExpr{
            op,
            std::make_unique<BoundColumnExpr>(0, leftType),
            std::make_unique<BoundColumnExpr>(1, rightType),
            DataType::Boolean};
    }

    void testTruthTables()
    {
        const std::array<Value, 3> values{true, false, std::monostate{}};
        using enum SqlTruth;
        const SqlTruth andResults[3][3] = {
            {True, False, Unknown},
            {False, False, False},
            {Unknown, False, Unknown}};
        const SqlTruth orResults[3][3] = {
            {True, True, True},
            {True, False, Unknown},
            {True, Unknown, Unknown}};
        const SqlTruth notResults[3] = {False, True, Unknown};

        const auto conjunction = binaryColumns(BinaryOperator::And);
        const auto disjunction = binaryColumns(BinaryOperator::Or);
        const BoundColumnExpr predicate{0, DataType::Boolean};
        const BoundUnaryExpr negation{
            UnaryOperator::Not, predicate.clone(), DataType::Boolean};

        for (std::size_t left = 0; left < values.size(); ++left)
        {
            const Row row{{values[left]}};
            require(evaluatePredicate(predicate, row) == (left == 0),
                    "WHERE must accept only TRUE");
            requireTruth(evaluateValue(negation, row), notResults[left],
                         "Incorrect NOT result");
            require(evaluatePredicate(negation, row) == (notResults[left] == True),
                    "WHERE NOT must exclude UNKNOWN");

            for (std::size_t right = 0; right < values.size(); ++right)
            {
                const Row operands{{values[left], values[right]}};
                const std::string context =
                    " at truth-table entry " + std::to_string(left) + "," +
                    std::to_string(right);
                requireTruth(evaluateValue(conjunction, operands), andResults[left][right],
                             "Incorrect AND result" + context);
                requireTruth(evaluateValue(disjunction, operands), orResults[left][right],
                             "Incorrect OR result" + context);
                require(evaluatePredicate(conjunction, operands) ==
                            (andResults[left][right] == True),
                        "Incorrect WHERE AND result" + context);
                require(evaluatePredicate(disjunction, operands) ==
                            (orResults[left][right] == True),
                        "Incorrect WHERE OR result" + context);
            }
        }

        const BoundLiteralExpr nullLiteral{std::monostate{}, DataType::Null};
        require(!evaluatePredicate(nullLiteral, Row{}), "WHERE NULL must reject the row");
    }

    void testComparisons()
    {
        struct Sample
        {
            Value lower;
            Value upper;
            DataType type;
        };
        const std::array samples{
            Sample{std::int32_t{1}, std::int32_t{2}, DataType::Int},
            Sample{std::int64_t{1}, std::int64_t{2}, DataType::BigInt},
            Sample{std::float32_t{1}, std::float32_t{2}, DataType::Float},
            Sample{std::float64_t{1}, std::float64_t{2}, DataType::Double},
            Sample{DecimalValue{10, 1}, DecimalValue{20, 1}, DataType::Decimal},
            Sample{std::string{"a"}, std::string{"b"}, DataType::Text},
            Sample{false, true, DataType::Boolean}};
        struct Comparison
        {
            BinaryOperator op;
            SqlTruth less;
            SqlTruth equal;
            SqlTruth greater;
        };
        using enum SqlTruth;
        const std::array comparisons{
            Comparison{BinaryOperator::Eq, False, True, False},
            Comparison{BinaryOperator::Ne, True, False, True},
            Comparison{BinaryOperator::Lt, True, False, False},
            Comparison{BinaryOperator::Le, True, True, False},
            Comparison{BinaryOperator::Gt, False, False, True},
            Comparison{BinaryOperator::Ge, False, True, True}};

        for (std::size_t left = 0; left < samples.size(); ++left)
        {
            for (std::size_t right = 0; right < samples.size(); ++right)
            {
                // Numeric types compare across representations; text and bool
                // are comparable only with their own types.
                if (left != right && (left >= 5 || right >= 5))
                {
                    continue;
                }
                for (const auto &comparison : comparisons)
                {
                    const auto expr = binaryColumns(
                        comparison.op, samples[left].type, samples[right].type);
                    requireTruth(evaluateValue(expr, Row{{samples[left].lower, samples[right].upper}}),
                                 comparison.less, "Incorrect less-than comparison");
                    requireTruth(evaluateValue(expr, Row{{samples[left].lower, samples[right].lower}}),
                                 comparison.equal, "Incorrect equal comparison");
                    requireTruth(evaluateValue(expr, Row{{samples[left].upper, samples[right].lower}}),
                                 comparison.greater, "Incorrect greater-than comparison");

                    for (const Row &row : {
                             Row{{std::monostate{}, samples[right].lower}},
                             Row{{samples[left].lower, std::monostate{}}},
                             Row{{std::monostate{}, std::monostate{}}}})
                    {
                        requireTruth(evaluateValue(expr, row), Unknown,
                                     "A NULL comparison must produce UNKNOWN");
                        require(!evaluatePredicate(expr, row),
                                "WHERE must exclude NULL comparisons");
                    }
                }
            }
        }
    }

    void testNestedExpressions()
    {
        const auto comparison = binaryColumns(BinaryOperator::Gt, DataType::Int, DataType::Int);
        const BoundUnaryExpr negation{
            UnaryOperator::Not, comparison.clone(), DataType::Boolean};
        const BoundBinaryExpr excludedMiddle{
            BinaryOperator::Or, comparison.clone(), negation.clone(), DataType::Boolean};
        const BoundBinaryExpr contradiction{
            BinaryOperator::And, comparison.clone(), negation.clone(), DataType::Boolean};
        const Row row{{std::monostate{}, std::int32_t{18}}};
        for (const BoundExpr *expr : std::array<const BoundExpr *, 3>{
                 &negation, &excludedMiddle, &contradiction})
        {
            requireTruth(evaluateValue(*expr, row), SqlTruth::Unknown,
                         "Nested logic must preserve UNKNOWN");
            require(!evaluatePredicate(*expr, row), "WHERE must exclude nested UNKNOWN");
        }

        const BoundIsNullExpr isNull{comparison.clone(), false};
        const BoundIsNullExpr isNotNull{comparison.clone(), true};
        requireTruth(evaluateValue(isNull, row), SqlTruth::True,
                     "UNKNOWN must be represented as NULL in Value");
        requireTruth(evaluateValue(isNotNull, row), SqlTruth::False,
                     "IS NOT NULL must reject UNKNOWN");
    }

    void testShortCircuitAndErrors()
    {
        const BoundBinaryExpr division{
            BinaryOperator::Divide,
            std::make_unique<BoundLiteralExpr>(std::int32_t{1}, DataType::Int),
            std::make_unique<BoundLiteralExpr>(std::int32_t{0}, DataType::Int),
            DataType::Int};
        const BoundBinaryExpr failingCondition{
            BinaryOperator::Eq, division.clone(),
            std::make_unique<BoundLiteralExpr>(std::int32_t{0}, DataType::Int),
            DataType::Boolean};
        for (const auto op : {BinaryOperator::And, BinaryOperator::Or})
        {
            const BoundBinaryExpr expr{
                op, std::make_unique<BoundColumnExpr>(0, DataType::Boolean),
                failingCondition.clone(), DataType::Boolean};
            const bool decisive = op == BinaryOperator::Or;
            requireTruth(evaluateValue(expr, Row{{decisive}}),
                         decisive ? SqlTruth::True : SqlTruth::False,
                         "A decisive left operand must short-circuit");
            for (const Value &left : {Value{!decisive}, Value{std::monostate{}}})
            {
                requireThrows([&] { evaluateValue(expr, Row{{left}}); }, "Division by zero");
            }

            const auto logical = binaryColumns(op);
            for (const Value &invalid : {Value{std::int32_t{1}}, Value{std::string{"true"}}})
            {
                requireThrows([&] { evaluateValue(logical, Row{{invalid, true}}); },
                              "Expected a boolean");
                for (const Value &left : {Value{!decisive}, Value{std::monostate{}}})
                {
                    requireThrows([&] { evaluateValue(logical, Row{{left, invalid}}); },
                                  "Expected a boolean");
                }
                const BoundLiteralExpr literal{invalid, DataType::Boolean};
                const BoundUnaryExpr negation{
                    UnaryOperator::Not, literal.clone(), DataType::Boolean};
                requireThrows([&] { evaluateValue(negation, Row{}); }, "Expected a boolean");
                requireThrows([&] { evaluatePredicate(literal, Row{}); }, "Expected a boolean");
            }
        }

        const auto comparison = binaryColumns(BinaryOperator::Eq, DataType::Int, DataType::Text);
        requireThrows(
            [&] { evaluateValue(comparison, Row{{std::int32_t{1}, std::string{"1"}}}); },
            "Cannot compare values of different types");
    }
}

int main()
{
    testTruthTables();
    testComparisons();
    testNestedExpressions();
    testShortCircuitAndErrors();
}
