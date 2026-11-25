#include "db_database.h"
#include "db_expression_evaluator.h"
#include "db_page_factory.h"
#include "db_query_exception.h"
#include "db_row_validator.h"
#include "db_test_directory.h"
#include "db_test_index.h"
#include "db_test_journal.h"
#include "db_write.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace
{
    void require(bool condition, const std::string &message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    template <typename Function>
    void requireError(Function function, const std::string &message)
    {
        try { function(); }
        catch (const std::exception &) { return; }
        throw std::runtime_error("Expected rejection: " + message);
    }

    void requireDate(const Value &value, std::int32_t expected, const std::string &message)
    {
        const auto *date = std::get_if<DateValue>(&value);
        require(date && date->daysSinceEpoch == expected, message);
    }

    void requireDateTimeOffset(const Value &value, std::int64_t milliseconds,
                               std::int32_t offset, const std::string &message)
    {
        const auto *datetime = std::get_if<DateTimeOffsetValue>(&value);
        // Check both stored fields explicitly: instant equality must not hide
        // a lost or incorrectly applied display offset during serialization.
        require(datetime && datetime->msSinceEpoch == milliseconds &&
                    datetime->minutesOffset == offset, message);
    }

    template <typename Function>
    void requireQueryError(Function function, const std::string &message,
                           QueryErrorCode code = QueryErrorCode::Validation)
    {
        try { function(); }
        catch (const QueryException &error)
        {
            require(error.code() == code, "Wrong query error category: " + message);
            return;
        }
        throw std::runtime_error("Expected query rejection: " + message);
    }

    // Expectations come from calendar/timezone rules, independently of the SQL
    // parser. Do not assume the machine running these tests uses UTC.
    DateValue comparisonDate()
    {
        using namespace std::chrono;
        return DateValue{static_cast<std::int32_t>(
            sys_days{2024y / January / 15}.time_since_epoch().count())};
    }

    DateTimeOffsetValue expectedLocalTime(std::chrono::local_time<std::chrono::milliseconds> local)
    {
        using namespace std::chrono;
        const auto *zone = current_zone();
        const auto utc = zone->to_sys(local);
        const auto offset = zone->get_info(utc).offset;
        require(offset == duration_cast<minutes>(offset), "Fixture requires a whole-minute UTC offset");
        return DateTimeOffsetValue{
            static_cast<std::int64_t>(utc.time_since_epoch().count()),
            static_cast<std::int32_t>(duration_cast<minutes>(offset).count())};
    }

    DateTimeOffsetValue comparisonMidnight()
    {
        using namespace std::chrono;
        return expectedLocalTime(local_days{2024y / January / 15});
    }

    Statement parse(const std::string &sql)
    {
        Lexer lexer{sql};
        Parser parser{lexer.tokenize()};
        return parser.parseStatement();
    }

    // Isolate binding from on-disk DATE layout support.
    class MemoryCatalog final : public Catalog
    {
    public:
        bool indexExists(const std::string &) const override { return false; }
        IndexHeaderPage getIndexHeader(const std::string &name) const override
        {
            throw QueryException(QueryErrorCode::Validation, "Index does not exist: " + name);
        }
        bool tableExists(const std::string &name) const override { return name == "events"; }
        HeaderPage getTableHeader(const std::string &) const override
        {
            HeaderPage header;
            header.tableName = "events";
            header.columns = {
                Column{"id", DataType::Int, false, 0, FixedColumnStorage{0, 4}},
                Column{"happened", DataType::Date, true, 1, FixedColumnStorage{4, 4}},
                Column{"label", DataType::Text, true, 2, VarColumnStorage{0}}};
            return header;
        }
    };

    class OffsetMemoryCatalog final : public Catalog
    {
    public:
        bool indexExists(const std::string &) const override { return false; }
        IndexHeaderPage getIndexHeader(const std::string &name) const override
        {
            throw QueryException(QueryErrorCode::Validation, "Index does not exist: " + name);
        }
        bool tableExists(const std::string &name) const override { return name == "events"; }
        HeaderPage getTableHeader(const std::string &) const override
        {
            HeaderPage header;
            header.tableName = "events";
            header.columns = {
                Column{"id", DataType::Int, false, 0, FixedColumnStorage{0, 4}},
                Column{"happened", DataType::Date, true, 1, FixedColumnStorage{4, 4}},
                Column{"occurred", DataType::DateTimeOffset, true, 2, FixedColumnStorage{8, 12}},
                Column{"other", DataType::DateTimeOffset, true, 3, FixedColumnStorage{20, 12}},
                Column{"label", DataType::Text, true, 4, VarColumnStorage{0}}};
            return header;
        }
    };

    BoundQuery bindSql(QueryValidator &validator, const std::string &sql)
    {
        return validator.validate(parse(sql));
    }

    void testConversion()
    {
        const std::array samples{
            std::pair{"1970-01-01", 0}, std::pair{"1969-12-31", -1},
            std::pair{"1970-01-02", 1}, std::pair{"2000-01-01", 10957},
            std::pair{"2000-02-29", 11016}, std::pair{"2026-11-18", 20775},
            std::pair{"0001-01-01", -719162}, std::pair{"9999-12-31", 2932896}};
        for (const auto &[text, expected] : samples)
        {
            require(Parser::dateToDays(text) == expected, "Wrong day count for " + std::string(text));
            std::ostringstream output;
            output << DateValue{expected};
            require(output.str() == text, "Date formatting changed the calendar date");
        }
        for (const std::string text : {"", "not-a-date", "2026/11/18", "2026-00-18", "2026-13-18",
                 "2026-11-00", "2026-11-31", "2026-02-29", "1900-02-29", "2100-02-29",
                 "2026-11-18x", "2026-11-18 12:00:00"})
            requireError([&] { Parser::dateToDays(text); }, text);
        require(Parser::dateToDays("2024-03-01") - Parser::dateToDays("2024-02-28") == 2, "Leap day was skipped");
        require(Parser::dateToDays("1900-03-01") - Parser::dateToDays("1900-02-28") == 1, "Century leap-year rule is wrong");
        require(DateValue{-1} < DateValue{0} && DateValue{0} == DateValue{0}, "Date ordering is not signed");
    }

    void testParser()
    {
        Lexer lexer{"SELECT DATE '1969-12-31', '1969-12-31';"};
        const auto tokens = lexer.tokenize();
        require(tokens[1].type == TokenType::Keyword && tokens[1].text == "DATE", "DATE is not a keyword");
        require(tokens[2].type == TokenType::String && tokens[4].type == TokenType::String,
                "Lexer inferred dates from string contents");
        const auto statement = parse("SELECT DATE '1969-12-31' AS d, '1969-12-31' AS text, 7 AS n;");
        const auto &select = std::get<SelectStatement>(statement);
        require(select.selectList.size() == 3, "Date parsing consumed the following item");
        const auto &first = dynamic_cast<const ExprSelectItem &>(*select.selectList[0]);
        const auto *date = dynamic_cast<const DateExpr *>(first.expr.get());
        require(date && date->value.daysSinceEpoch == -1 && first.alias == "d", "Explicit date literal parsed incorrectly");
        const auto &second = dynamic_cast<const ExprSelectItem &>(*select.selectList[1]);
        require(dynamic_cast<const StringExpr *>(second.expr.get()), "Plain string became a date expression");
        (void)parse("SELECT (DATE '1970-01-01') = DATE '1970-01-01';");
        (void)parse("INSERT INTO events VALUES (1, DATE '2000-02-29', 'leap');");
        for (const std::string sql : {"SELECT DATE 123;", "SELECT DATE;", "SELECT DATE NULL;", "SELECT DATE '2026-02-30';"})
            requireError([&] { parse(sql); }, sql);
    }

    void testBinding()
    {
        MemoryCatalog catalog;
        QueryValidator validator{catalog};
        const auto inserted = std::get<BoundInsert>(bindSql(validator,
            "INSERT INTO events VALUES (1, '1970-01-01', '1970-01-01'), (2, DATE '1969-12-31', 'old'), (3, NULL, NULL);"));
        requireDate(inserted.rows[0].values[1], 0, "Implicit INSERT date was not converted");
        requireDate(inserted.rows[1].values[1], -1, "Explicit INSERT date changed type or value");
        require(std::get<std::string>(inserted.rows[0].values[2]) == "1970-01-01", "TEXT column was implicitly converted");
        require(std::holds_alternative<std::monostate>(inserted.rows[2].values[1]), "NULL DATE was not preserved");
        const auto select = std::get<BoundSelect>(bindSql(validator, "SELECT DATE '1970-01-01', '1970-01-01' FROM events;"));
        require(select.projections[0].expr->type() == DataType::Date && select.projections[1].expr->type() == DataType::Text,
                "Projection lost DATE/TEXT type distinction");
        require(select.projections[0].outputName == "DATE '1970-01-01'", "DATE projection label is incorrect");
        const auto text = std::get<BoundSelect>(bindSql(validator, "SELECT '2026-02-30' FROM events;"));
        require(std::get<std::string>(evaluateValue(*text.projections[0].expr, Row{})) == "2026-02-30",
                "A plain text literal was validated as a date");
        for (const std::string sql : {"UPDATE events SET happened = '1970-01-01';",
                                     "UPDATE events SET happened = DATE '1970-01-01';"})
        {
            const auto update = std::get<BoundUpdate>(bindSql(validator, sql));
            require(update.assignments[0].expr->type() == DataType::Date, "UPDATE literal was not converted to DATE");
            requireDate(evaluateValue(*update.assignments[0].expr, Row{}), 0, "UPDATE converted to the wrong day");
        }
        for (const std::string predicate : {"happened = '1970-01-01'", "'1970-01-01' = happened",
                 "DATE '1970-01-01' = '1970-01-01'", "'1970-01-01' = DATE '1970-01-01'"})
        {
            const auto query = std::get<BoundSelect>(bindSql(validator, "SELECT id FROM events WHERE " + predicate + ';'));
            const auto &comparison = dynamic_cast<const BoundBinaryExpr &>(*query.where);
            require(comparison.left->type() == DataType::Date && comparison.right->type() == DataType::Date,
                    "Comparison coercion is asymmetric or requires a column");
            require(evaluatePredicate(*query.where, Row{{std::int32_t{1}, DateValue{0}, std::string{"x"}}}),
                    "Bound date predicate did not evaluate correctly");
        }
        for (const std::string sql : {"INSERT INTO events VALUES (1, '2026-02-30', 'bad');",
                 "INSERT INTO events VALUES (1, 0, 'bad');", "INSERT INTO events VALUES (1, TRUE, 'bad');",
                 "INSERT INTO events VALUES (DATE '1970-01-01', NULL, 'bad');",
                 "INSERT INTO events VALUES (1, NULL, DATE '1970-01-01');",
                 "SELECT id FROM events WHERE happened = 'not-a-date';",
                 "SELECT id FROM events WHERE happened = id;", "SELECT id FROM events WHERE happened = label;",
                 "UPDATE events SET happened = 0;", "UPDATE events SET happened = '2026-02-30';",
                 "SELECT happened + 1 FROM events;", "SELECT happened * 2 FROM events;",
                 "SELECT SUM(happened) FROM events;", "SELECT ABS(happened) FROM events;"})
            requireError([&] { bindSql(validator, sql); }, sql);
    }

    void testEvaluator()
    {
        struct Comparison { BinaryOperator op; bool less; bool equal; bool greater; };
        const std::array cases{
            Comparison{BinaryOperator::Eq, false, true, false}, Comparison{BinaryOperator::Ne, true, false, true},
            Comparison{BinaryOperator::Lt, true, false, false}, Comparison{BinaryOperator::Le, true, true, false},
            Comparison{BinaryOperator::Gt, false, false, true}, Comparison{BinaryOperator::Ge, false, true, true}};
        for (const auto &test : cases)
        {
            const BoundBinaryExpr expr{test.op, std::make_unique<BoundColumnExpr>(0, DataType::Date),
                std::make_unique<BoundColumnExpr>(1, DataType::Date), DataType::Boolean};
            for (const auto &[row, expected] : std::array{
                std::pair{Row{{DateValue{-1}, DateValue{0}}}, test.less},
                std::pair{Row{{DateValue{0}, DateValue{0}}}, test.equal},
                std::pair{Row{{DateValue{0}, DateValue{-1}}}, test.greater}})
                require(std::get<bool>(evaluateValue(expr, row)) == expected, "Incorrect date comparison");
            for (const Row &row : {Row{{std::monostate{}, DateValue{0}}}, Row{{DateValue{0}, std::monostate{}}}})
                require(std::holds_alternative<std::monostate>(evaluateValue(expr, row)) && !evaluatePredicate(expr, row),
                        "Date/NULL comparison did not produce UNKNOWN");
            requireError([&] { evaluateValue(expr, Row{{DateValue{0}, std::int32_t{0}}}); }, "Date/integer comparison");
            requireError([&] { evaluateValue(expr, Row{{DateValue{0}, std::string{"1970-01-01"}}}); }, "Unconverted date/text comparison");
        }
    }

    void testStorage()
    {
        const RowSchema schema{buildStoredColumns({
            Column{"d", DataType::Date, true, 0, FixedColumnStorage{}},
            Column{"label", DataType::Text, true, 1, VarColumnStorage{}}})};
        require(std::get<FixedColumnStorage>(schema.columns[0].storage).size == 4, "DATE is not stored in four bytes");
        for (const Value &date : {Value{DateValue{-719162}}, Value{DateValue{-1}}, Value{DateValue{0}},
                                 Value{DateValue{2932896}}, Value{std::monostate{}}})
        {
            const Row row{{date, std::string{"variable payload"}}};
            require(validateRowAgainstSchema(schema.columns, row).valid, "DATE row validation failed");
            ByteWriter writer(RowWriter::computeSerializedRowSize(schema, row.values));
            RowWriter(writer, schema).writeRow(row.values);
            ByteReader reader(writer.bytes());
            require(RowDecoder(reader, schema, 0).decodeRow().values == row.values, "DATE row round-trip failed");
        }
        require(!validateRowAgainstSchema(schema.columns, Row{{std::int32_t{0}, std::string{"wrong"}}}).valid,
                "DATE row validator accepted a plain integer");
        const auto bytes = ValueSerializer::serializeValue(DataType::Date, DateValue{-1});
        require(bytes.size() == 4, "Standalone DATE encoding has wrong size");
        ByteReader reader(bytes);
        requireDate(ValueDeserializer::decodeFixed(reader, 0, DataType::Date), -1, "Standalone DATE round-trip failed");
    }

    std::set<std::int32_t> ids(const QueryResult &result)
    {
        std::set<std::int32_t> values;
        for (const auto &row : result.rows) values.insert(std::get<std::int32_t>(row.values[0]));
        return values;
    }

    void testSql()
    {
        TestDirectory directory{"db-date-sql"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "dates");
            db.executeSql("CREATE TABLE events (id INT, happened DATE, label TEXT);");
            db.executeSql("INSERT INTO events VALUES (1, DATE '1969-12-31', 'old'), (2, '1970-01-01', 'epoch'), (3, NULL, 'missing'), (4, '1970-01-02', 'next');");
            const std::array predicates{
                std::pair{"happened < '1970-01-01'", std::set<std::int32_t>{1}},
                std::pair{"'1970-01-01' < happened", std::set<std::int32_t>{4}},
                std::pair{"happened <= DATE '1970-01-01'", std::set<std::int32_t>{1, 2}},
                std::pair{"happened >= '1970-01-01'", std::set<std::int32_t>{2, 4}},
                std::pair{"happened <> '1970-01-01'", std::set<std::int32_t>{1, 4}},
                std::pair{"happened IS NULL", std::set<std::int32_t>{3}}};
            for (const auto &[predicate, expected] : predicates)
                require(ids(db.executeSql("SELECT id FROM events WHERE " + std::string(predicate) + ';')) == expected,
                        "Wrong date filtering for " + std::string(predicate));
            db.executeSql("UPDATE events SET happened = '2000-02-29' WHERE id = 1;");
            db.executeSql("UPDATE events SET happened = DATE '2000-02-29' WHERE id = 2;");
            db.executeSql("UPDATE events SET happened = happened WHERE id = 4;");
            const auto groups = db.executeSql("SELECT happened, COUNT(*) FROM events GROUP BY happened HAVING happened = '2000-02-29';");
            require(groups.rows.size() == 1 && std::get<std::int64_t>(groups.rows[0].values[1]) == 2,
                    "DATE GROUP BY/HAVING failed");
            const auto before = journal_test::readFile(root / "tables/events.table");
            requireError([&] { db.executeSql("UPDATE events SET happened = '2026-02-30' WHERE id = 1;"); }, "Invalid UPDATE date");
            require(journal_test::readFile(root / "tables/events.table") == before, "Invalid UPDATE changed stored rows");
            require(db.executeSql("DELETE FROM events WHERE happened < '1970-01-03';").affectedRows == 1, "DATE DELETE predicate failed");
            requireError([&] { db.executeSql("SELECT happened FROM events WHERE happened = label;"); }, "DATE compared to TEXT column");
        }
        auto db = Database::open(root, "dates");
        const auto result = db.executeSql("SELECT happened FROM events WHERE id = 1;");
        require(result.columns[0].type == DataType::Date, "Reopened schema lost DATE type");
        requireDate(result.rows[0].values[0], 11016, "Date changed after reopening");
    }

    void testDefaultsAndConstraints()
    {
        TestDirectory directory{"db-date-defaults"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "dates");
            db.executeSql("CREATE TABLE events (id INT, happened DATE NOT NULL DEFAULT '1970-01-01', explicit_date DATE DEFAULT DATE '1969-12-31', CHECK (happened >= DATE '1970-01-01'));");
            db.executeSql("INSERT INTO events (id) VALUES (1);");
            const auto result = db.executeSql("SELECT happened, explicit_date FROM events;");
            requireDate(result.rows[0].values[0], 0, "Implicit DATE default failed");
            requireDate(result.rows[0].values[1], -1, "Explicit DATE default failed");
            requireError([&] { db.executeSql("INSERT INTO events (id, happened) VALUES (2, NULL);"); }, "NOT NULL date");
            requireError([&] { db.executeSql("INSERT INTO events (id, happened) VALUES (2, '1969-12-31');"); }, "DATE CHECK constraint");
        }
        auto db = Database::open(root, "dates");
        db.executeSql("INSERT INTO events (id) VALUES (2);");
        requireError([&] { db.executeSql("UPDATE events SET happened = DATE '1969-12-31';"); }, "Persisted DATE CHECK constraint");
        require(db.executeSql("SELECT * FROM events;").rows.size() == 2, "Persisted date defaults failed");
    }

    void testIndexes()
    {
        TestDirectory directory{"db-date-index"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "dates");
            db.executeSql("CREATE TABLE events (id INT, happened DATE);");
            db.executeSql("INSERT INTO events VALUES (1, '1970-01-02'), (2, '1969-12-31'), (3, NULL);");
            db.executeSql("CREATE UNIQUE INDEX dates ON events (happened);");
            db.executeSql("INSERT INTO events VALUES (4, DATE '1970-01-01'), (5, NULL);");
            requireError([&] { db.executeSql("INSERT INTO events VALUES (6, '1970-01-01');"); }, "Duplicate DATE index key");
            db.executeSql("UPDATE events SET happened = '2000-02-29' WHERE id = 2;");
            db.executeSql("DELETE FROM events WHERE happened = '1970-01-02';");
        }
        index_test::Tree tree(root, "dates", true);
        require(tree.entries.size() == 4, "DATE index was not maintained");
        require(std::holds_alternative<std::monostate>(tree.entries[0].key.values[0]) &&
                std::holds_alternative<std::monostate>(tree.entries[1].key.values[0]), "NULL DATE index ordering failed");
        requireDate(tree.entries[2].key.values[0], 0, "DATE index order is incorrect");
        requireDate(tree.entries[3].key.values[0], 11016, "DATE index update is missing");
        tree.verifyRouting();
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        auto index = Index::open(LinuxDirectory(root / "indexes"), "dates.index", recovery);
        auto cursor = index.seekLowerBound(Row{{DateValue{0}}});
        const auto entry = cursor.next();
        require(entry.has_value(), "DATE index seek returned no entry");
        requireDate(entry->key.values[0], 0, "DATE index seek failed");
    }

    void testOffsetConversion()
    {
        struct Sample { const char *text; std::int64_t milliseconds; std::int32_t offset; };
        const Sample samples[]{
            {"1970-01-01 00:00:00+00:00", 0, 0},
            {"1970-01-01 01:00:00+01:00", 0, 60},
            {"1969-12-31 18:30:00-05:30", 0, -330},
            {"1970-01-01 05:45:00+05:45", 0, 345},
            {"1970-01-01 00:00:00+01:00", -3'600'000, 60},
            {"1970-01-01 00:00:00-05:30", 19'800'000, -330},
            {"1970-01-01 00:00:00.123+00:00", 123, 0},
            {"1969-12-31 23:59:59.999+00:00", -1, 0},
            {"2000-02-29 00:00:00+00:00", 951'782'400'000, 0},
            {"2038-01-19 03:14:08+00:00", 2'147'483'648'000, 0}};
        for (const auto &sample : samples)
        {
            const auto [milliseconds, offset] = Parser::dateTimeOffsetToMilliseconds(sample.text);
            require(milliseconds == sample.milliseconds && offset == sample.offset,
                    "Incorrect UTC conversion or offset: " + std::string(sample.text));
        }

        using namespace std::chrono;
        for (const auto month : {January, July})
        {
            const local_time<milliseconds> local = local_days{2024y / month / 15} +
                hours{12} + minutes{34} + seconds{56} + milliseconds{789};
            std::ostringstream text;
            text << local;
            const auto expected = expectedLocalTime(local);
            const auto [actual, offset] = Parser::dateTimeOffsetToMilliseconds(text.str());
            require(actual == expected.msSinceEpoch && offset == expected.minutesOffset,
                    "Offsetless datetime did not use the timezone rules for its date");
        }

        for (const std::string text : {"", "invalid", "2024-01-15", "2024-02-30 12:00:00+00:00",
                 "1900-02-29 12:00:00+00:00", "2024-01-15 25:00:00+00:00",
                 "2024-01-15 12:60:00+00:00", "2024-01-15 12:00:00+broken",
                 "2024-01-15 12:00:00+01:00junk", "2024-01-15 12:00:00 junk"})
            requireQueryError([&] { Parser::dateTimeOffsetToMilliseconds(text); }, text);

        // Exercise gaps and folds when the machine's zone has transitions.
        // Fixed-offset zones have no ambiguous/nonexistent local times to test.
        const auto *zone = current_zone();
        auto cursor = sys_seconds{sys_days{2024y / January / 1}};
        const auto end = sys_seconds{sys_days{2025y / January / 1}};
        while (cursor < end)
        {
            const auto before = zone->get_info(cursor);
            if (before.end >= end) break;
            const auto after = zone->get_info(before.end);
            if (before.offset != after.offset)
            {
                const auto middle = std::min(before.offset, after.offset) +
                    abs(after.offset - before.offset) / 2;
                const local_time<milliseconds> local{
                    duration_cast<milliseconds>(before.end.time_since_epoch() + middle)};
                std::ostringstream text;
                text << local;
                requireQueryError([&] { Parser::dateTimeOffsetToMilliseconds(text.str()); },
                                  "Local time in a timezone gap or fold");
            }
            require(before.end > cursor, "Timezone fixture did not advance");
            cursor = before.end;
        }
    }

    void testOffsetParser()
    {
        for (const std::string keyword : {"DATETIMEOFFSET", "DATETIME"})
        {
            Lexer lexer{"SELECT " + keyword + " '1970-01-01 01:00:00.123+01:00';"};
            require(lexer.tokenize()[1].type == TokenType::Keyword, keyword + " is not a keyword");
            const auto statement = parse("SELECT " + keyword +
                " '1970-01-01 01:00:00.123+01:00' AS t, '1970-01-01 01:00:00+01:00' AS text;");
            const auto &select = std::get<SelectStatement>(statement);
            require(select.selectList.size() == 2, "Timestamp parsing consumed the next item");
            const auto &item = dynamic_cast<const ExprSelectItem &>(*select.selectList[0]);
            const auto *timestamp = dynamic_cast<const DateTimeOffsetExpr *>(item.expr.get());
            require(timestamp && timestamp->value.msSinceEpoch == 123 &&
                        timestamp->value.minutesOffset == 60 && item.alias == "t",
                    "Timestamp literal lost its instant, offset, or alias");
            const auto &text = dynamic_cast<const ExprSelectItem &>(*select.selectList[1]);
            require(dynamic_cast<const StringExpr *>(text.expr.get()), "Lexer/parser inferred a timestamp from TEXT");

            const auto creation = parse("CREATE TABLE events (occurred " + keyword + ");");
            const auto &column = std::get<CreateTableStatement>(creation).columns.at(0);
            require(column->dataType->type == DataType::DateTimeOffset, "Timestamp column type was not parsed");
        }
        for (const std::string sql : {"SELECT DATETIMEOFFSET;", "SELECT DATETIMEOFFSET 123;",
                                     "SELECT DATETIMEOFFSET NULL;"})
            requireQueryError([&] { parse(sql); }, sql, QueryErrorCode::Syntax);
        requireQueryError([&] { parse("SELECT DATETIMEOFFSET '2024-02-30 12:00:00+00:00';"); },
                          "Invalid timestamp literal");
    }

    void testOffsetBinding()
    {
        OffsetMemoryCatalog catalog;
        QueryValidator validator{catalog};
        const auto insert = std::get<BoundInsert>(bindSql(validator,
            "INSERT INTO events (id, occurred, label) VALUES "
            "(1, '1970-01-01 01:00:00.123+01:00', '1970-01-01'), "
            "(2, DATETIMEOFFSET '1969-12-31 18:30:00-05:30', 'text'), (3, NULL, NULL);"));
        requireDateTimeOffset(insert.rows[0].values[2], 123, 60, "Implicit timestamp INSERT lost its type/offset");
        requireDateTimeOffset(insert.rows[1].values[2], 0, -330, "Explicit timestamp INSERT was converted twice");
        require(std::holds_alternative<std::monostate>(insert.rows[2].values[2]), "Timestamp NULL was converted");
        require(std::get<std::string>(insert.rows[0].values[4]) == "1970-01-01", "TEXT assignment became temporal");

        const auto select = std::get<BoundSelect>(bindSql(validator,
            "SELECT DATETIMEOFFSET '1970-01-01 01:00:00+01:00' AS t, 'not-a-date' AS text FROM events;"));
        require(select.projections[0].expr->type() == DataType::DateTimeOffset &&
                    select.projections[1].expr->type() == DataType::Text, "Projection lost timestamp/TEXT types");
        requireDateTimeOffset(evaluateValue(*select.projections[0].expr, Row{}), 0, 60,
                              "Timestamp projection was not bound correctly");

        for (const std::string value : {"'2024-01-15'", "'2024-01-15 00:00:00'"})
        {
            const auto update = std::get<BoundUpdate>(bindSql(validator, "UPDATE events SET occurred = " + value + ';'));
            const auto midnight = comparisonMidnight();
            requireDateTimeOffset(evaluateValue(*update.assignments[0].expr, Row{}),
                                  midnight.msSinceEpoch, midnight.minutesOffset,
                                  "Date-only/local timestamp UPDATE did not become local midnight");
        }
        for (const std::string sql : {"UPDATE events SET occurred = 1;",
                 "UPDATE events SET occurred = 'invalid';",
                 "INSERT INTO events (id, occurred) VALUES (1, 0);",
                 "SELECT occurred + 1 FROM events;", "SELECT ABS(occurred) FROM events;",
                 "SELECT SUM(occurred) FROM events;"})
            requireQueryError([&] { bindSql(validator, sql); }, sql);
    }

    struct TemporalComparison
    {
        BinaryOperator op;
        const char *sql;
        bool less;
        bool equal;
        bool greater;
    };

    constexpr std::array temporalComparisons{
        TemporalComparison{BinaryOperator::Eq, "=", false, true, false},
        TemporalComparison{BinaryOperator::Ne, "<>", true, false, true},
        TemporalComparison{BinaryOperator::Lt, "<", true, false, false},
        TemporalComparison{BinaryOperator::Le, "<=", true, true, false},
        TemporalComparison{BinaryOperator::Gt, ">", false, false, true},
        TemporalComparison{BinaryOperator::Ge, ">=", false, true, true}};

    void requireComparison(const TemporalComparison &comparison, DataType leftType, DataType rightType,
                           const Value &left, const Value &right, bool expected, const std::string &message)
    {
        const BoundBinaryExpr expr{comparison.op,
            std::make_unique<BoundColumnExpr>(0, leftType),
            std::make_unique<BoundColumnExpr>(1, rightType), DataType::Boolean};
        require(std::get<bool>(evaluateValue(expr, Row{{left, right}})) == expected,
                message + " (" + comparison.sql + ")");
        for (const Row &row : {Row{{std::monostate{}, right}}, Row{{left, std::monostate{}}},
                              Row{{std::monostate{}, std::monostate{}}}})
            require(std::holds_alternative<std::monostate>(evaluateValue(expr, row)) && !evaluatePredicate(expr, row),
                    "Temporal comparison with NULL did not produce UNKNOWN");
    }

    void testOffsetEvaluator()
    {
        for (const auto &comparison : temporalComparisons)
        {
            // Display offsets must not participate in instant equality/order.
            requireComparison(comparison, DataType::DateTimeOffset, DataType::DateTimeOffset,
                DateTimeOffsetValue{0, 60}, DateTimeOffsetValue{0, -330}, comparison.equal,
                "Same instant with different offsets compared differently");
            requireComparison(comparison, DataType::DateTimeOffset, DataType::DateTimeOffset,
                DateTimeOffsetValue{-1, 840}, DateTimeOffsetValue{0, -720}, comparison.less,
                "Timestamp comparison used local clock time or unsigned milliseconds");
            requireComparison(comparison, DataType::DateTimeOffset, DataType::DateTimeOffset,
                DateTimeOffsetValue{1, -720}, DateTimeOffsetValue{0, 840}, comparison.greater,
                "Timestamp comparison lost millisecond precision");
        }
    }

    void testCrossTypeEvaluator()
    {
        const auto date = comparisonDate();
        const auto midnight = comparisonMidnight().msSinceEpoch;
        for (const auto &comparison : temporalComparisons)
        {
            for (const auto &[delta, expected, reversed] : std::array{
                     std::tuple<std::int64_t, bool, bool>{-1, comparison.greater, comparison.less},
                     std::tuple<std::int64_t, bool, bool>{0, comparison.equal, comparison.equal},
                     std::tuple<std::int64_t, bool, bool>{1, comparison.less, comparison.greater}})
            {
                const DateTimeOffsetValue timestamp{midnight + delta, 120};
                requireComparison(comparison, DataType::Date, DataType::DateTimeOffset,
                                  date, timestamp, expected, "DATE/DATETIMEOFFSET comparison failed");
                requireComparison(comparison, DataType::DateTimeOffset, DataType::Date,
                                  timestamp, date, reversed, "DATETIMEOFFSET/DATE comparison failed");
            }
            requireComparison(comparison, DataType::Date, DataType::DateTimeOffset,
                date, DateTimeOffsetValue{midnight + 43'200'000, -330}, comparison.less,
                "Cross-type comparison discarded the time of day");
        }
    }

    void testCrossTypeBinding()
    {
        OffsetMemoryCatalog catalog;
        QueryValidator validator{catalog};
        struct Predicate { const char *sql; DataType left; DataType right; };
        const Predicate predicates[]{
            {"happened = occurred", DataType::Date, DataType::DateTimeOffset},
            {"occurred = happened", DataType::DateTimeOffset, DataType::Date},
            {"happened = '2024-01-15 00:00:00'", DataType::Date, DataType::DateTimeOffset},
            {"'2024-01-15 00:00:00' = happened", DataType::DateTimeOffset, DataType::Date},
            {"occurred = '2024-01-15'", DataType::DateTimeOffset, DataType::Date},
            {"'2024-01-15' = occurred", DataType::Date, DataType::DateTimeOffset},
            {"occurred = DATE '2024-01-15'", DataType::DateTimeOffset, DataType::Date},
            {"DATE '2024-01-15' = occurred", DataType::Date, DataType::DateTimeOffset},
            {"happened = DATETIMEOFFSET '2024-01-15 00:00:00'", DataType::Date, DataType::DateTimeOffset},
            {"DATETIMEOFFSET '2024-01-15 00:00:00' = happened", DataType::DateTimeOffset, DataType::Date},
            {"occurred = '2024-01-15 00:00:00'", DataType::DateTimeOffset, DataType::DateTimeOffset},
            {"'2024-01-15 00:00:00' = occurred", DataType::DateTimeOffset, DataType::DateTimeOffset},
            {"happened = '2024-01-15'", DataType::Date, DataType::Date}};
        const auto midnight = comparisonMidnight();
        const Row row{{std::int32_t{1}, comparisonDate(), midnight, midnight, std::string{"2024-01-15"}}};
        const auto matchesBoundType = [](const BoundExpr &expr, DataType expected)
        {
            // A DATE literal may be promoted during binding; DATE columns stay DATE.
            return expr.type() == expected ||
                   (expected == DataType::Date && expr.type() == DataType::DateTimeOffset &&
                    expr.kind() == BoundExprKind::Literal);
        };
        for (const auto &predicate : predicates)
        {
            const auto query = std::get<BoundSelect>(bindSql(validator,
                "SELECT id FROM events WHERE " + std::string(predicate.sql) + ';'));
            const auto &comparison = dynamic_cast<const BoundBinaryExpr &>(*query.where);
            require(matchesBoundType(*comparison.left, predicate.left) && matchesBoundType(*comparison.right, predicate.right),
                    "Temporal literal classification depended on operand order: " + std::string(predicate.sql));
            require(evaluatePredicate(*query.where, row), "Cross-type binding/evaluation failed: " + std::string(predicate.sql));
        }
        for (const std::string predicate : {"happened = NULL", "NULL = happened", "occurred = NULL", "NULL = occurred"})
        {
            const auto query = std::get<BoundSelect>(bindSql(validator, "SELECT id FROM events WHERE " + predicate + ';'));
            require(std::holds_alternative<std::monostate>(evaluateValue(*query.where, row)),
                    "Binder rejected or converted a temporal NULL comparison");
        }
        const auto strings = std::get<BoundSelect>(bindSql(validator,
            "SELECT '2024-01-15' = '2024-01-15 00:00:00' AS same FROM events;"));
        const auto &comparison = dynamic_cast<const BoundBinaryExpr &>(*strings.projections[0].expr);
        require(comparison.left->type() == DataType::Text && comparison.right->type() == DataType::Text &&
                    !std::get<bool>(evaluateValue(comparison, Row{})), "Ordinary TEXT comparison was coerced to temporal values");
        for (const std::string predicate : {"occurred = id", "id = occurred", "occurred = TRUE",
                 "occurred = label", "label = occurred", "happened = label", "happened = id",
                 "occurred = 'not-a-date'", "happened = '2024-02-30 00:00:00+00:00'"})
            requireQueryError([&] { bindSql(validator, "SELECT id FROM events WHERE " + predicate + ';'); }, predicate);
    }

    void testOffsetStorage()
    {
        const RowSchema schema{buildStoredColumns({
            Column{"occurred", DataType::DateTimeOffset, true, 0, FixedColumnStorage{}},
            Column{"day", DataType::Date, true, 1, FixedColumnStorage{}},
            Column{"label", DataType::Text, true, 2, VarColumnStorage{}}})};
        require(std::get<FixedColumnStorage>(schema.columns[0].storage).size == 12,
                "DATETIMEOFFSET needs eight bytes for milliseconds and four for offset minutes");
        require(std::get<FixedColumnStorage>(schema.columns[1].storage).offset == 12,
                "Column following DATETIMEOFFSET overlaps its offset field");
        for (const auto &timestamp : std::array{DateTimeOffsetValue{-1, -330}, DateTimeOffsetValue{0, 60},
                                               DateTimeOffsetValue{2'147'483'648'123, 345}})
        {
            const Row row{{timestamp, DateValue{-1}, std::string{"payload"}}};
            require(validateRowAgainstSchema(schema.columns, row).valid, "DATETIMEOFFSET row validation failed");
            ByteWriter writer(RowWriter::computeSerializedRowSize(schema, row.values));
            RowWriter(writer, schema).writeRow(row.values);
            ByteReader reader(writer.bytes());
            const auto decoded = RowDecoder(reader, schema, 0).decodeRow();
            requireDateTimeOffset(decoded.values[0], timestamp.msSinceEpoch, timestamp.minutesOffset,
                                  "Timestamp row round-trip lost precision or offset");
            requireDate(decoded.values[1], -1, "Timestamp layout overwrote adjacent DATE");
            require(std::get<std::string>(decoded.values[2]) == "payload", "Timestamp layout overwrote variable data");

            const auto bytes = ValueSerializer::serializeValue(DataType::DateTimeOffset, timestamp);
            require(bytes.size() == 12, "Standalone timestamp encoding has wrong size");
            ByteReader valueReader(bytes);
            requireDateTimeOffset(ValueDeserializer::decodeFixed(valueReader, 0, DataType::DateTimeOffset),
                                  timestamp.msSinceEpoch, timestamp.minutesOffset, "Standalone timestamp round-trip failed");
        }
        const Row nullRow{{std::monostate{}, DateValue{0}, std::string{"null timestamp"}}};
        require(validateRowAgainstSchema(schema.columns, nullRow).valid, "Nullable timestamp rejected NULL");
        ByteWriter writer(RowWriter::computeSerializedRowSize(schema, nullRow.values));
        RowWriter(writer, schema).writeRow(nullRow.values);
        ByteReader reader(writer.bytes());
        require(std::holds_alternative<std::monostate>(RowDecoder(reader, schema, 0).decodeRow().values[0]),
                "NULL timestamp was stored as the epoch");
        for (const Value &wrong : {Value{DateValue{0}}, Value{std::int64_t{0}}, Value{std::string{"1970-01-01"}}})
            require(!validateRowAgainstSchema(schema.columns, Row{{wrong, DateValue{0}, std::string{}}}).valid,
                    "Storage validator silently coerced a non-timestamp value");
    }

    void testOffsetSql()
    {
        TestDirectory directory{"db-offset-sql"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "timestamps");
            db.executeSql("CREATE TABLE events (id INT, occurred DATETIMEOFFSET, label TEXT);");
            db.executeSql("INSERT INTO events VALUES "
                "(1, '1970-01-01 01:00:00+01:00', 'east'), "
                "(2, DATETIMEOFFSET '1969-12-31 18:30:00-05:30', 'west'), "
                "(3, '1970-01-01 00:00:00.001+00:00', 'after'), "
                "(4, '1969-12-31 23:59:59.999+00:00', 'before'), (5, NULL, 'missing');");
            const std::array predicates{
                std::pair{"occurred = '1970-01-01 00:00:00+00:00'", std::set<std::int32_t>{1, 2}},
                std::pair{"'1970-01-01 00:00:00+00:00' = occurred", std::set<std::int32_t>{1, 2}},
                std::pair{"occurred < '1970-01-01 00:00:00+00:00'", std::set<std::int32_t>{4}},
                std::pair{"occurred >= '1970-01-01 00:00:00+00:00'", std::set<std::int32_t>{1, 2, 3}},
                std::pair{"occurred <> '1970-01-01 00:00:00+00:00'", std::set<std::int32_t>{3, 4}},
                std::pair{"occurred IS NULL", std::set<std::int32_t>{5}}};
            for (const auto &[predicate, expected] : predicates)
                require(ids(db.executeSql("SELECT id FROM events WHERE " + std::string(predicate) + ';')) == expected,
                        "Timestamp WHERE used an offset as part of equality/order: " + std::string(predicate));
            const auto groups = db.executeSql("SELECT COUNT(*) AS n FROM events GROUP BY occurred "
                "HAVING occurred = '1970-01-01 00:00:00+00:00';");
            require(groups.rows.size() == 1 && std::get<std::int64_t>(groups.rows[0].values[0]) == 2,
                    "GROUP BY split one instant by its display offsets");
            db.executeSql("UPDATE events SET occurred = '1970-01-01 03:00:00+02:00' WHERE id = 3;");
            const auto before = journal_test::readFile(root / "tables/events.table");
            requireQueryError([&] { db.executeSql("UPDATE events SET occurred = '2024-02-30 12:00:00+00:00';"); },
                              "Invalid timestamp UPDATE");
            require(journal_test::readFile(root / "tables/events.table") == before, "Rejected timestamp UPDATE changed data");
            require(db.executeSql("DELETE FROM events WHERE occurred < '1970-01-01 00:00:00+00:00';").affectedRows == 1,
                    "Timestamp DELETE used local time instead of UTC");
        }
        auto db = Database::open(root, "timestamps");
        const auto original = db.executeSql("SELECT occurred FROM events WHERE id = 1;");
        require(original.columns[0].type == DataType::DateTimeOffset, "Reopened schema lost DATETIMEOFFSET");
        requireDateTimeOffset(original.rows[0].values[0], 0, 60, "Reopened timestamp lost its original offset");
        requireDateTimeOffset(db.executeSql("SELECT occurred FROM events WHERE id = 2;").rows[0].values[0],
                              0, -330, "Negative offset did not survive reopening");
        requireDateTimeOffset(db.executeSql("SELECT occurred FROM events WHERE id = 3;").rows[0].values[0],
                              3'600'000, 120, "Timestamp UPDATE did not survive reopening");
    }

    void testCrossTypeSql()
    {
        TestDirectory directory{"db-cross-temporal-sql"};
        auto db = Database::create(directory.path / "db", "timestamps");
        db.executeSql("CREATE TABLE events (id INT, happened DATE, occurred DATETIMEOFFSET);");
        db.executeSql("INSERT INTO events VALUES "
            "(1, '2024-01-15', '2024-01-15 00:00:00'), "
            "(2, '2024-01-15', '2024-01-15 12:00:00'), "
            "(3, '2024-01-15', '2024-01-14 23:59:59.999'), "
            "(4, NULL, '2024-01-15 00:00:00'), (5, '2024-01-15', NULL);");
        for (const auto &comparison : temporalComparisons)
        {
            std::set<std::int32_t> forward;
            std::set<std::int32_t> reverse;
            if (comparison.equal) { forward.insert(1); reverse.insert(1); }
            if (comparison.less) { forward.insert(2); reverse.insert(3); }
            if (comparison.greater) { forward.insert(3); reverse.insert(2); }
            require(ids(db.executeSql("SELECT id FROM events WHERE happened " + std::string(comparison.sql) + " occurred;")) == forward,
                    "DATE-column/DATETIMEOFFSET-column comparison failed");
            require(ids(db.executeSql("SELECT id FROM events WHERE occurred " + std::string(comparison.sql) + " happened;")) == reverse,
                    "DATETIMEOFFSET-column/DATE-column comparison failed");
        }
        for (const std::string predicate : {"happened = '2024-01-15 00:00:00'",
                 "'2024-01-15 00:00:00' = happened", "happened < '2024-01-15 12:00:00'"})
            require(ids(db.executeSql("SELECT id FROM events WHERE " + predicate + ';')) == std::set<std::int32_t>{1, 2, 3, 5},
                    "Datetime string compared to a DATE column incorrectly");
        for (const std::string predicate : {"occurred = '2024-01-15'", "'2024-01-15' = occurred"})
            require(ids(db.executeSql("SELECT id FROM events WHERE " + predicate + ';')) == std::set<std::int32_t>{1, 4},
                    "Date-only string did not mean local midnight for a timestamp comparison");
        const auto unknown = db.executeSql("SELECT happened = occurred AS same FROM events WHERE id = 4;");
        require(std::holds_alternative<std::monostate>(unknown.rows[0].values[0]), "Cross-type SQL NULL did not produce UNKNOWN");
        require(ids(db.executeSql("SELECT a.id FROM events AS a JOIN events AS b ON a.happened = b.occurred WHERE b.id = 1;")) ==
                    std::set<std::int32_t>{1, 2, 3, 5}, "JOIN did not apply cross-type temporal comparison");
    }

    void testOffsetDefaultsAndConstraints()
    {
        TestDirectory directory{"db-offset-defaults"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "timestamps");
            db.executeSql("CREATE TABLE events (id INT, "
                "occurred DATETIMEOFFSET NOT NULL DEFAULT '1970-01-01 01:00:00+01:00', "
                "other DATETIMEOFFSET DEFAULT DATETIMEOFFSET '1969-12-31 18:30:00-05:30', "
                "CHECK (occurred >= DATETIMEOFFSET '1970-01-01 00:00:00+00:00'));");
            db.executeSql("INSERT INTO events (id) VALUES (1);");
            requireQueryError([&] { db.executeSql("INSERT INTO events (id, occurred) VALUES (2, NULL);"); },
                              "Timestamp NOT NULL", QueryErrorCode::Constraint);
            requireQueryError([&] { db.executeSql("INSERT INTO events (id, occurred) VALUES "
                "(2, '1969-12-31 23:59:59.999+00:00');"); }, "Timestamp CHECK", QueryErrorCode::Constraint);
            db.executeSql("CREATE TABLE mixed (id INT, happened DATE DEFAULT '2024-01-15', "
                "occurred DATETIMEOFFSET DEFAULT '2024-01-15', CHECK (occurred >= happened));");
            db.executeSql("INSERT INTO mixed (id) VALUES (1);");
        }
        auto db = Database::open(root, "timestamps");
        db.executeSql("INSERT INTO events (id) VALUES (3);");
        for (const auto &row : db.executeSql("SELECT occurred, other FROM events;").rows)
        {
            requireDateTimeOffset(row.values[0], 0, 60, "Persisted implicit timestamp default failed");
            requireDateTimeOffset(row.values[1], 0, -330, "Persisted explicit timestamp default failed");
        }
        db.executeSql("INSERT INTO mixed (id) VALUES (2);");
        const auto midnight = comparisonMidnight();
        requireDateTimeOffset(db.executeSql("SELECT occurred FROM mixed WHERE id = 2;").rows[0].values[0],
                              midnight.msSinceEpoch, midnight.minutesOffset, "Date-only timestamp default lost local midnight");
        requireQueryError([&] { db.executeSql("UPDATE mixed SET occurred = '2024-01-14 23:59:59.999';"); },
                          "Persisted cross-type CHECK", QueryErrorCode::Constraint);
        require(db.executeSql("SELECT * FROM mixed;").rows.size() == 2, "Rejected cross-type UPDATE lost rows");
    }

    void testOffsetIndexes()
    {
        TestDirectory directory{"db-offset-index"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "timestamps");
            db.executeSql("CREATE TABLE events (id INT, occurred DATETIMEOFFSET);");
            db.executeSql("INSERT INTO events VALUES (1, '1970-01-01 01:00:00+01:00'), "
                "(2, '1970-01-01 00:00:00.001+00:00'), (3, NULL);");
            db.executeSql("CREATE UNIQUE INDEX timestamps ON events (occurred);");
            requireQueryError([&] { db.executeSql("INSERT INTO events VALUES (6, '1970-01-01 00:00:00+00:00');"); },
                              "Same instant with another offset in a UNIQUE index", QueryErrorCode::Constraint);
            db.executeSql("INSERT INTO events VALUES (4, '1969-12-31 18:30:01-05:30'), (5, NULL);");
            db.executeSql("UPDATE events SET occurred = '1969-12-31 23:59:59.999+00:00' WHERE id = 2;");
            db.executeSql("DELETE FROM events WHERE occurred = '1970-01-01 00:00:00+00:00';");

            db.executeSql("CREATE TABLE duplicates (occurred DATETIMEOFFSET);");
            db.executeSql("INSERT INTO duplicates VALUES ('1970-01-01 00:00:00+00:00'), ('1970-01-01 01:00:00+01:00');");
            requireQueryError([&] { db.executeSql("CREATE UNIQUE INDEX duplicate_instants ON duplicates (occurred);"); },
                              "Bulk-loaded index accepted duplicate instants", QueryErrorCode::Constraint);
            require(!std::filesystem::exists(root / "indexes/duplicate_instants.index"), "Rejected timestamp index left a file");
        }
        index_test::Tree tree(root, "timestamps", true);
        require(tree.entries.size() == 4, "Timestamp index was not maintained after rejected insert, UPDATE, and DELETE");
        require(std::holds_alternative<std::monostate>(tree.entries[0].key.values[0]) &&
                    std::holds_alternative<std::monostate>(tree.entries[1].key.values[0]), "NULL timestamp index order is incorrect");
        requireDateTimeOffset(tree.entries[2].key.values[0], -1, 0, "Timestamp index lost updated negative milliseconds");
        requireDateTimeOffset(tree.entries[3].key.values[0], 1000, -330, "Timestamp index lost its display offset");
        tree.verifyRouting();
        const journal_test::DatabaseIdentity identity{root};
        StatementRecovery recovery{LinuxDirectory(root), identity.bytes};
        auto index = Index::open(LinuxDirectory(root / "indexes"), "timestamps.index", recovery);
        auto cursor = index.seekLowerBound(Row{{DateTimeOffsetValue{1000, 120}}});
        const auto entry = cursor.next();
        require(entry.has_value(), "Index seek skipped an equal instant with a different offset");
        requireDateTimeOffset(entry->key.values[0], 1000, -330, "Index seek compared display offsets");
    }
}

int main(int argc, char **argv)
{
    const std::map<std::string, void (*)()> tests{
        {"conversion", testConversion}, {"parser", testParser}, {"binding", testBinding}, {"evaluator", testEvaluator},
        {"storage", testStorage}, {"sql", testSql}, {"defaults", testDefaultsAndConstraints}, {"indexes", testIndexes},
        {"offset_conversion", testOffsetConversion}, {"offset_parser", testOffsetParser},
        {"offset_binding", testOffsetBinding}, {"offset_evaluator", testOffsetEvaluator},
        {"offset_storage", testOffsetStorage}, {"offset_sql", testOffsetSql},
        {"offset_defaults", testOffsetDefaultsAndConstraints}, {"offset_indexes", testOffsetIndexes},
        {"cross_binding", testCrossTypeBinding}, {"cross_evaluator", testCrossTypeEvaluator}, {"cross_sql", testCrossTypeSql}};
    try
    {
        if (argc != 2) throw std::runtime_error("Pass one date test group");
        tests.at(argv[1])();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
