#include "db_constraint_validator.h"
#include "db_page_file.h"
#include "db_sql_lexer.h"
#include "db_storage_engine.h"
#include "db_query_validator.h"
#include "db_test_directory.h"
#include "db_write.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    // Set up real stored tables without using the unfinished UPDATE writer.
    void executeSetup(StorageEngine &engine, const std::string &sql)
    {
        Lexer lexer{sql};
        Parser parser{lexer.tokenize()};
        const auto statement = parser.parseStatement();
        QueryValidator binder{engine};
        const auto query = binder.validate(statement);
        engine.beginStatement();
        try
        {
            std::visit([&](const auto &bound)
            {
                using T = std::decay_t<decltype(bound)>;
                if constexpr (std::is_same_v<T, BoundCreateTable>)
                    engine.createTable(bound);
                else if constexpr (std::is_same_v<T, BoundInsert>)
                    engine.insertRows(bound);
                else if constexpr (std::is_same_v<T, BoundDelete>)
                    engine.deleteRows(bound);
                else
                    throw std::runtime_error("Unsupported fixture statement");
            }, query);
        }
        catch (...)
        {
            engine.rollbackStatement();
            throw;
        }
        engine.commitStatement();
    }

    std::set<RowId> locateRows(StorageEngine &engine, const std::string &tableName,
                               const std::set<std::int32_t> &values, ColumnId columnId = 0)
    {
        auto table = engine.openTable(tableName);
        auto cursor = table.scan();
        std::set<RowId> ids;
        while (auto row = cursor.nextLocated())
        {
            if (values.contains(std::get<std::int32_t>(row->row.values.at(columnId))))
                ids.insert(row->id);
        }
        require(ids.size() == values.size(), "Fixture did not locate every original row");
        return ids;
    }

    ConstraintValidationResult validateUpdate(
        StorageEngine &engine, const std::string &tableName,
        const std::vector<std::string> &tableNames, const std::vector<Row> &newRows,
        const std::set<RowId> &excludedRows)
    {
        StorageConstraintLookup lookup{engine};
        ConstraintValidator validator{lookup};
        return validator.validateUpdateConstraint(
            engine.getTableHeader(tableName), newRows, excludedRows, tableNames, engine);
    }

    void requireForeignKeyFailure(const ConstraintValidationResult &result,
                                  const std::string &message)
    {
        require(!result.valid && result.constraintName.has_value() && result.message &&
                    result.message->find("FOREIGN KEY") != std::string::npos,
                message);
    }

    void testParentFinalState()
    {
        TestDirectory directory{"db-update-parent-state"};
        auto engine = StorageEngine::create(directory.path / "db");
        executeSetup(engine, "CREATE TABLE parents (id INT PRIMARY KEY, label TEXT);");
        executeSetup(engine, "CREATE TABLE children (id INT PRIMARY KEY, parent_id INT, "
                             "FOREIGN KEY (parent_id) REFERENCES parents (id));");
        executeSetup(engine, "INSERT INTO parents VALUES (1, 'p'), (2, 'q'), (3, 'r');");
        executeSetup(engine, "INSERT INTO children VALUES (10, 1);");
        const std::vector<std::string> tables{"parents", "children"};
        const auto first = locateRows(engine, "parents", {1});
        const auto second = locateRows(engine, "parents", {2});
        const auto both = locateRows(engine, "parents", {1, 2});

        require(validateUpdate(engine, "parents", tables, {Row{{1, std::string{"changed"}}}}, first).valid,
                "An unchanged referenced key or its own PK blocked UPDATE");
        requireForeignKeyFailure(
            validateUpdate(engine, "parents", tables, {Row{{4, std::string{"p"}}}}, first),
            "Changing a referenced key left an orphan child");
        require(validateUpdate(engine, "parents", tables, {Row{{4, std::string{"q"}}}}, second).valid,
                "A reference to an unaffected parent blocked UPDATE");
        require(validateUpdate(engine, "parents", tables,
                               {Row{{2, std::string{"p"}}}, Row{{1, std::string{"q"}}}}, both).valid,
                "A key supplied by another replacement row was not found");
        require(!validateUpdate(engine, "parents", tables,
                                {Row{{3, std::string{"q"}}}}, second).valid,
                "UPDATE ignored an unaffected row's duplicate primary key");
        require(!validateUpdate(engine, "parents", tables,
                                {Row{{1, std::string{"p"}}}, Row{{1, std::string{"q"}}}}, both).valid,
                "UPDATE ignored duplicate keys in its replacement batch");

        // Parent and child can have identical physical IDs in different tables.
        require(first == locateRows(engine, "children", {10}),
                "Fixture did not exercise table-local row ID collisions");
        const auto child = locateRows(engine, "children", {10});
        require(validateUpdate(engine, "children", tables, {Row{{10, 1}}}, child).valid,
                "Child exclusions incorrectly hid a parent in another table");
        require(validateUpdate(engine, "children", tables, {Row{{10, 2}}}, child).valid,
                "Changing an outgoing FK to an existing parent was rejected");
        requireForeignKeyFailure(validateUpdate(engine, "children", tables, {Row{{10, 99}}}, child),
                                 "UPDATE accepted an outgoing FK without a parent");
    }

    void testCompositeForeignKeys()
    {
        TestDirectory directory{"db-update-composite-fk"};
        auto engine = StorageEngine::create(directory.path / "db");
        executeSetup(engine, "CREATE TABLE parents (label TEXT, a INT, b INT, PRIMARY KEY (a, b));");
        executeSetup(engine, "INSERT INTO parents VALUES ('p', 10, 20), ('q', 11, 21);");
        executeSetup(engine, "CREATE TABLE children (id INT PRIMARY KEY, y INT, label TEXT, x INT, "
                             "FOREIGN KEY (x, y) REFERENCES parents (b, a));");
        executeSetup(engine, "INSERT INTO children VALUES "
                             "(1, 10, 'p', 20), (2, 11, 'q', 21), "
                             "(3, 999, 'null_x', NULL), (4, NULL, 'null_y', 999);");
        const std::vector<std::string> tables{"parents", "children"};
        const auto first = locateRows(engine, "parents", {10}, 1);
        require(validateUpdate(engine, "parents", tables,
                               {Row{{std::string{"changed"}, 10, 20}}}, first).valid,
                "Composite FK column mapping or child NULL handling was incorrect");
        requireForeignKeyFailure(
            validateUpdate(engine, "parents", tables, {Row{{std::string{"p"}, 10, 21}}}, first),
            "A partial composite key incorrectly satisfied a child reference");
        require(validateUpdate(engine, "parents", tables,
                               {Row{{std::string{"p"}, 11, 21}}, Row{{std::string{"q"}, 10, 20}}},
                               locateRows(engine, "parents", {10, 11}, 1)).valid,
                "Swapping complete composite keys incorrectly broke references");
    }

    void testNullableParentKeys()
    {
        TestDirectory directory{"db-update-nullable-fk"};
        auto engine = StorageEngine::create(directory.path / "db");
        executeSetup(engine, "CREATE TABLE parents (id INT PRIMARY KEY, a INT, b INT, UNIQUE (a, b));");
        executeSetup(engine, "INSERT INTO parents VALUES (1, NULL, 7), (2, 7, NULL), (3, 7, 8);");
        executeSetup(engine, "CREATE TABLE children (x INT, y INT, "
                             "FOREIGN KEY (x, y) REFERENCES parents (a, b));");
        executeSetup(engine, "INSERT INTO children VALUES (NULL, 7), (7, NULL), (NULL, NULL), (7, 8);");
        const std::vector<std::string> tables{"parents", "children"};
        require(validateUpdate(engine, "parents", tables, {Row{{1, 99, 7}}},
                               locateRows(engine, "parents", {1})).valid,
                "A child containing NULL incorrectly referenced a nullable parent key");
        requireForeignKeyFailure(
            validateUpdate(engine, "parents", tables, {Row{{3, std::monostate{}, 8}}},
                           locateRows(engine, "parents", {3})),
            "Replacing a referenced key with NULL left an orphan child");
    }

    void testIndependentIncomingReferences()
    {
        TestDirectory directory{"db-update-independent-fk"};
        auto engine = StorageEngine::create(directory.path / "db");
        executeSetup(engine, "CREATE TABLE parents (id INT PRIMARY KEY, code INT UNIQUE);");
        executeSetup(engine, "INSERT INTO parents VALUES (1, NULL), (2, 20), (3, 30), (4, 40);");
        executeSetup(engine, "CREATE TABLE children (parent_code INT, parent_id INT, "
                             "FOREIGN KEY (parent_code) REFERENCES parents (code), "
                             "FOREIGN KEY (parent_id) REFERENCES parents (id));");
        executeSetup(engine, "INSERT INTO children VALUES (NULL, 1), (20, NULL);");
        executeSetup(engine, "CREATE TABLE other_children (parent_id INT, "
                             "FOREIGN KEY (parent_id) REFERENCES parents (id));");
        executeSetup(engine, "INSERT INTO other_children VALUES (3);");
        executeSetup(engine, "CREATE TABLE unrelated_parents (id INT PRIMARY KEY);");
        executeSetup(engine, "INSERT INTO unrelated_parents VALUES (4);");
        executeSetup(engine, "CREATE TABLE unrelated_children (parent_id INT, "
                             "FOREIGN KEY (parent_id) REFERENCES unrelated_parents (id));");
        executeSetup(engine, "INSERT INTO unrelated_children VALUES (4);");
        const std::vector<std::string> tables{
            "parents", "children", "other_children", "unrelated_parents", "unrelated_children"};
        requireForeignKeyFailure(
            validateUpdate(engine, "parents", tables, {Row{{5, std::monostate{}}}},
                           locateRows(engine, "parents", {1})),
            "NULL in one FK skipped another incoming reference");
        requireForeignKeyFailure(
            validateUpdate(engine, "parents", tables, {Row{{2, 25}}}, locateRows(engine, "parents", {2})),
            "An incoming reference to a UNIQUE key was ignored");
        requireForeignKeyFailure(
            validateUpdate(engine, "parents", tables, {Row{{5, 30}}}, locateRows(engine, "parents", {3})),
            "An incoming reference from another child table was ignored");
        require(validateUpdate(engine, "parents", tables, {Row{{5, 40}}},
                               locateRows(engine, "parents", {4})).valid,
                "An FK referencing an unrelated table blocked UPDATE");
    }

    void testSelfReferences()
    {
        TestDirectory directory{"db-update-self-fk"};
        const auto root = directory.path / "db";
        {
            auto engine = StorageEngine::create(root);
            executeSetup(engine, "CREATE TABLE nodes (id INT PRIMARY KEY, parent_id INT);");
            executeSetup(engine, "INSERT INTO nodes VALUES (1, 1), (2, 1), (3, 3);");
        }
        // CREATE binding does not support self-references yet. Seed the FK
        // metadata directly so the update validator's final-state logic is tested.
        {
            PageFile file{LinuxDirectory{root / "tables"}, "nodes.table"};
            auto page = decodeHeaderPage(file.readPage(HeaderPageId));
            auto &schema = std::get<HeaderPage>(page.data);
            schema.constraints.push_back(BoundForeignKeyConstraintExpr{
                "nodes_parent", {1}, "nodes", {0}});
            file.writePage(HeaderPageId, encodeHeaderPage(page.header, schema));
            file.sync();
        }
        auto engine = StorageEngine::open(root);
        const std::vector<std::string> tables{"nodes"};
        const auto both = locateRows(engine, "nodes", {1, 2});
        require(validateUpdate(engine, "nodes", tables, {Row{{10, 10}}, Row{{20, 10}}}, both).valid,
                "Replacement children could not reference replacement parents");
        requireForeignKeyFailure(
            validateUpdate(engine, "nodes", tables, {Row{{10, 10}}}, locateRows(engine, "nodes", {1})),
            "An unaffected self-referencing child lost its parent");
        requireForeignKeyFailure(
            validateUpdate(engine, "nodes", tables, {Row{{10, 1}}, Row{{20, 10}}}, both),
            "A replacement child incorrectly referenced an excluded original parent");
        require(validateUpdate(engine, "nodes", tables,
                               {Row{{10, std::monostate{}}}, Row{{20, 3}}}, both).valid,
                "NULL or references to unaffected self-parents were rejected");
    }
}

int main()
{
    try
    {
        testParentFinalState();
        testCompositeForeignKeys();
        testNullableParentKeys();
        testIndependentIncomingReferences();
        testSelfReferences();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
