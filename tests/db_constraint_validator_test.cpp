#include "db_constraint_validator.h"
#include "db_database.h"
#include "db_linux_file.h"
#include "db_test_directory.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    std::vector<std::byte> readFile(const std::filesystem::path &path)
    {
        LinuxFile file{LinuxDirectory{path.parent_path()}, path.filename(),
                       LinuxFile::OpenMode::ReadOnly};
        std::vector<std::byte> bytes(static_cast<std::size_t>(file.size()));
        file.readExactAt(0, bytes);
        return bytes;
    }

    template <typename Function>
    void rejectBatch(Database &database, const std::filesystem::path &root,
                     const std::string &table, Function statement,
                     const std::string &expectedError)
    {
        const auto tablePath = root / "tables" / (table + ".table");
        const auto before = readFile(tablePath);
        const auto rowCount = database.executeSql("SELECT * FROM " + table + ";").rows.size();
        bool rejected = false;
        try
        {
            statement();
        }
        catch (const std::runtime_error &error)
        {
            require(std::string{error.what()}.find(expectedError) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            rejected = true;
        }
        require(rejected, "Expected statement rejection: " + expectedError);
        require(readFile(tablePath) == before, "Rejected statement changed table bytes");
        require(database.executeSql("SELECT * FROM " + table + ";").rows.size() == rowCount,
                "Rejected statement changed the row count");
        require(!std::filesystem::exists(root / "journal/journal.log"),
                "Rejected statement left an active journal");
    }

    void testPrimaryKeysAndReopen()
    {
        TestDirectory directory{"db-constraint-primary"};
        const auto root = directory.path / "db";
        {
            auto database = Database::create(root, "test");
            // Constraints on the same columns must have independent seen sets.
            database.executeSql("CREATE TABLE items (id INT PRIMARY KEY UNIQUE);");
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES (1), (2), (1);");
            }, "PRIMARY KEY");
            const auto result = database.executeSql("INSERT INTO items VALUES (1), (2);");
            require(result.affectedRows == 2, "Successful INSERT has incorrect affectedRows");
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES (3), (1);");
            }, "PRIMARY KEY");
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES (3), (4), (3);");
            }, "PRIMARY KEY");
            database.executeSql("INSERT INTO items VALUES (3);");
        }
        {
            auto database = Database::open(root, "test");
            const auto rows = database.executeSql("SELECT * FROM items;").rows;
            require(rows.size() == 3, "Successful batches did not survive reopening");
            for (std::size_t i = 0; i < rows.size(); ++i)
                require(std::get<std::int32_t>(rows[i].values[0]) == static_cast<std::int32_t>(i + 1),
                        "Unexpected persisted primary key");
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES (4), (2);");
            }, "PRIMARY KEY");
            database.executeSql("INSERT INTO items VALUES (4), (5);");
            require(database.executeSql("SELECT * FROM items;").rows.size() == 5,
                    "A rejection prevented the next valid statement");
        }
    }

    void testCompositeAndIndependentKeys()
    {
        TestDirectory directory{"db-constraint-composite"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE pairs (a INT, b INT, PRIMARY KEY (a, b), "
                            "UNIQUE (a, b), UNIQUE (b, a));");
        database.executeSql("INSERT INTO pairs VALUES (1, 2), (1, 3), (2, 2), (2, 1);");
        rejectBatch(database, root, "pairs", [&]
        {
            database.executeSql("INSERT INTO pairs VALUES (3, 1), (3, 2), (3, 1);");
        }, "PRIMARY KEY");
        rejectBatch(database, root, "pairs", [&]
        {
            database.executeSql("INSERT INTO pairs VALUES (3, 1), (1, 2);");
        }, "PRIMARY KEY");

        database.executeSql("CREATE TABLE independent (a INT UNIQUE, b INT UNIQUE);");
        database.executeSql("INSERT INTO independent VALUES (1, 2), (2, 1);");
        rejectBatch(database, root, "independent", [&]
        {
            database.executeSql("INSERT INTO independent VALUES (3, 4), (3, 5);");
        }, "UNIQUE");
        rejectBatch(database, root, "independent", [&]
        {
            database.executeSql("INSERT INTO independent VALUES (3, 4), (5, 4);");
        }, "UNIQUE");
    }

    void testUniqueNulls()
    {
        TestDirectory directory{"db-constraint-unique"};
        const auto root = directory.path / "db";
        {
            auto database = Database::create(root, "test");
            database.executeSql("CREATE TABLE items (code TEXT UNIQUE, a INT, b INT, UNIQUE (a, b));");
            database.executeSql("INSERT INTO items VALUES "
                                "(NULL, 1, NULL), (NULL, 1, NULL), "
                                "('first', NULL, 2), ('second', NULL, 2), "
                                "(NULL, NULL, NULL), (NULL, NULL, NULL), "
                                "('third', 1, 2), ('fourth', 1, 3);");
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES ('new', 4, 5), ('first', 6, 7);");
            }, "UNIQUE");
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES ('new', 4, 5), ('new', 6, 7);");
            }, "UNIQUE");
            // A NULL in one constraint must not skip another constraint.
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES (NULL, 4, 5), (NULL, 4, 5);");
            }, "UNIQUE");
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES (NULL, 4, 5), (NULL, 1, 2);");
            }, "UNIQUE");
        }
        auto reopened = Database::open(root, "test");
        reopened.executeSql("INSERT INTO items VALUES (NULL, 1, NULL), (NULL, NULL, 2);");
        require(reopened.executeSql("SELECT * FROM items;").rows.size() == 10,
                "Stored NULLs incorrectly conflict with a subsequent batch");
        rejectBatch(reopened, root, "items", [&]
        {
            reopened.executeSql("INSERT INTO items VALUES ('first', NULL, NULL);");
        }, "UNIQUE");
    }

    void testInvalidSqlRows()
    {
        TestDirectory directory{"db-constraint-invalid-sql"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE items (id INT PRIMARY KEY, label TEXT NOT NULL);");
        database.executeSql("INSERT INTO items VALUES (1, 'existing');");
        for (const std::string invalid : {"(3)", "(3, 'label', 4)"})
        {
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES (2, 'new'), " + invalid + ";");
            }, "column count");
        }
        rejectBatch(database, root, "items", [&]
        {
            database.executeSql("INSERT INTO items VALUES (2, 'new'), ('bad', 'label');");
        }, "Expected numeric value");
        for (const std::string invalid : {"(NULL, 'label')", "(3, NULL)"})
        {
            rejectBatch(database, root, "items", [&]
            {
                database.executeSql("INSERT INTO items VALUES (2, 'new'), " + invalid + ";");
            }, "NOT NULL");
        }
        database.executeSql("INSERT INTO items VALUES (2, 'new');");
        require(database.executeSql("SELECT * FROM items;").rows.size() == 2,
                "Malformed rows prevented a later valid INSERT");
    }

    void testOtherConstraints()
    {
        TestDirectory directory{"db-constraint-other"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE parents (label TEXT, a INT, b INT, PRIMARY KEY (a, b));");
        database.executeSql("INSERT INTO parents VALUES ('p', 10, 20), ('q', 11, 21);");
        database.executeSql("CREATE TABLE children (id INT PRIMARY KEY, x INT, y INT, "
                            "quantity INT DEFAULT 3, optional INT NULL, "
                            "FOREIGN KEY (x, y) REFERENCES parents (b, a), "
                            "CHECK (quantity > 0));");
        database.executeSql("INSERT INTO children (id, x, y) VALUES (1, 20, 10), (2, 20, 10);");
        database.executeSql("INSERT INTO children VALUES "
                            "(3, NULL, 999, NULL, NULL), (4, 999, NULL, 5, 7);");
        const auto rows = database.executeSql("SELECT * FROM children;").rows;
        require(std::get<std::int32_t>(rows[0].values[3]) == 3 &&
                    std::holds_alternative<std::monostate>(rows[2].values[3]),
                "DEFAULT or CHECK UNKNOWN semantics changed");
        rejectBatch(database, root, "children", [&]
        {
            database.executeSql("INSERT INTO children (id, x, y) VALUES (5, 21, 11), (6, 20, 11);");
        }, "FOREIGN KEY");
        rejectBatch(database, root, "children", [&]
        {
            database.executeSql("INSERT INTO children VALUES "
                                "(5, 21, 11, 1, NULL), (6, NULL, NULL, -1, NULL);");
        }, "CHECK");
        database.executeSql("INSERT INTO children (id, x, y) VALUES (5, 21, 11);");
    }

    void testForeignKeyDeleteRestrictions()
    {
        TestDirectory directory{"db-constraint-delete-references"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE parents (id INT PRIMARY KEY);");
        database.executeSql("CREATE TABLE children (id INT PRIMARY KEY, parent_id INT, "
                            "FOREIGN KEY (parent_id) REFERENCES parents (id));");
        database.executeSql("INSERT INTO parents VALUES (1), (2), (3);");

        require(database.executeSql("DELETE FROM parents WHERE id = 1;").affectedRows == 1,
                "An empty child table blocked parent deletion");
        database.executeSql("INSERT INTO children VALUES (10, 3);");
        rejectBatch(database, root, "parents", [&]
        {
            database.executeSql("DELETE FROM parents WHERE id = 3;");
        }, "FOREIGN KEY");
        require(database.executeSql("DELETE FROM parents WHERE id = 2;").affectedRows == 1,
                "An unrelated child row blocked parent deletion");
        require(database.executeSql("DELETE FROM parents WHERE id = 99;").affectedRows == 0,
                "A DELETE matching no parents did not return zero affectedRows");

        // Each DELETE must enumerate incoming FKs, even after earlier scans.
        rejectBatch(database, root, "parents", [&]
        {
            database.executeSql("DELETE FROM parents WHERE id = 3;");
        }, "FOREIGN KEY");
        require(database.executeSql("DELETE FROM children WHERE id = 10;").affectedRows == 1,
                "An outgoing FK blocked child deletion");
        require(database.executeSql("DELETE FROM parents WHERE id = 3;").affectedRows == 1,
                "Removing the child did not unblock parent deletion");
        require(database.executeSql("SELECT * FROM parents;").rows.empty(),
                "Successful DELETE statements left parent rows behind");
    }

    void testForeignKeyDeleteCompositeKeys()
    {
        TestDirectory directory{"db-constraint-delete-composite"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE parents (label TEXT, a INT, b INT, PRIMARY KEY (a, b));");
        database.executeSql("INSERT INTO parents VALUES "
                            "('referenced', 10, 20), ('same_a', 10, 21), "
                            "('same_b', 11, 20), ('reversed', 20, 10);");
        // Local positions differ from parent positions, and REFERENCES reverses
        // the primary key order. A match must compare the entire mapped tuple.
        database.executeSql("CREATE TABLE children (id INT PRIMARY KEY, label TEXT, y INT, x INT, "
                            "FOREIGN KEY (x, y) REFERENCES parents (b, a));");
        database.executeSql("INSERT INTO children VALUES (1, 'child', 10, 20);");
        rejectBatch(database, root, "parents", [&]
        {
            database.executeSql("DELETE FROM parents WHERE a = 10 AND b = 20;");
        }, "FOREIGN KEY");
        require(database.executeSql("DELETE FROM parents WHERE b = 21 OR a = 11 OR a = 20;")
                    .affectedRows == 3,
                "Partial or reversed composite keys incorrectly blocked deletion");
        const auto rows = database.executeSql("SELECT * FROM parents;").rows;
        require(rows.size() == 1 && std::get<std::string>(rows[0].values[0]) == "referenced",
                "Composite-key deletion retained the wrong parent");
    }

    void testForeignKeyDeleteNulls()
    {
        TestDirectory directory{"db-constraint-delete-nulls"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE parents (id INT PRIMARY KEY, a INT, b INT, UNIQUE (a, b));");
        database.executeSql("INSERT INTO parents VALUES "
                            "(1, NULL, 7), (2, 7, NULL), (3, NULL, NULL), (4, 7, 8), (5, 9, 10);");
        database.executeSql("CREATE TABLE children (id INT PRIMARY KEY, x INT, y INT, "
                            "FOREIGN KEY (x, y) REFERENCES parents (a, b));");
        database.executeSql("INSERT INTO children VALUES "
                            "(10, NULL, 7), (11, 7, NULL), (12, NULL, NULL), "
                            "(13, 7, 8), (14, NULL, 10), (15, 9, NULL);");

        // Skipping a NULL-containing parent key must not skip later parents.
        rejectBatch(database, root, "parents", [&]
        {
            database.executeSql("DELETE FROM parents WHERE id <= 4;");
        }, "FOREIGN KEY");
        require(database.executeSql("DELETE FROM parents WHERE id <= 3;").affectedRows == 3,
                "NULL-containing parent keys incorrectly matched child NULLs");
        require(database.executeSql("DELETE FROM parents WHERE id = 5;").affectedRows == 1,
                "A NULL-containing child key incorrectly referenced a non-NULL parent");
        rejectBatch(database, root, "parents", [&]
        {
            database.executeSql("DELETE FROM parents WHERE id = 4;");
        }, "FOREIGN KEY");
        require(database.executeSql("SELECT * FROM children;").rows.size() == 6,
                "Parent deletion changed child rows");
    }

    void testForeignKeyDeleteIndependentReferences()
    {
        TestDirectory directory{"db-constraint-delete-independent"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE parents (id INT PRIMARY KEY, code INT UNIQUE);");
        database.executeSql("INSERT INTO parents VALUES (1, NULL), (2, 20), (3, 30), (4, 40);");
        database.executeSql("CREATE TABLE children (id INT PRIMARY KEY, parent_code INT, parent_id INT, "
                            "FOREIGN KEY (parent_code) REFERENCES parents (code), "
                            "FOREIGN KEY (parent_id) REFERENCES parents (id));");
        database.executeSql("INSERT INTO children VALUES (10, NULL, 1), (11, 20, NULL);");
        database.executeSql("CREATE TABLE other_children (id INT PRIMARY KEY, parent_id INT, "
                            "FOREIGN KEY (parent_id) REFERENCES parents (id));");
        database.executeSql("INSERT INTO other_children VALUES (12, 3);");

        // A NULL in one referenced key must not skip another incoming FK.
        // References through either key or either child table block deletion.
        for (const std::string id : {"1", "2", "3"})
        {
            rejectBatch(database, root, "parents", [&]
            {
                database.executeSql("DELETE FROM parents WHERE id = " + id + ";");
            }, "FOREIGN KEY");
        }

        database.executeSql("CREATE TABLE unrelated_parents (id INT PRIMARY KEY);");
        database.executeSql("INSERT INTO unrelated_parents VALUES (4);");
        database.executeSql("CREATE TABLE unrelated_children (parent_id INT, "
                            "FOREIGN KEY (parent_id) REFERENCES unrelated_parents (id));");
        database.executeSql("INSERT INTO unrelated_children VALUES (4);");
        require(database.executeSql("DELETE FROM parents WHERE id = 4;").affectedRows == 1,
                "An FK referencing another table incorrectly blocked deletion");
    }

    void testForeignKeyDeleteBatchAndReopen()
    {
        TestDirectory directory{"db-constraint-delete-batch"};
        const auto root = directory.path / "db";
        std::vector<std::byte> parentBytes;
        std::vector<std::byte> childBytes;
        {
            auto database = Database::create(root, "test");
            database.executeSql("CREATE TABLE parents (id INT PRIMARY KEY, label TEXT);");
            database.executeSql("INSERT INTO parents VALUES "
                                "(1, 'unreferenced first'), (2, 'referenced'), (3, 'unreferenced last');");
            database.executeSql("CREATE TABLE children (id INT PRIMARY KEY, parent_id INT, "
                                "FOREIGN KEY (parent_id) REFERENCES parents (id));");
            database.executeSql("INSERT INTO children VALUES (10, 2), (11, 2);");
            parentBytes = readFile(root / "tables/parents.table");
            childBytes = readFile(root / "tables/children.table");

            rejectBatch(database, root, "parents", [&]
            {
                database.executeSql("DELETE FROM parents;");
            }, "FOREIGN KEY");
            require(readFile(root / "tables/children.table") == childBytes,
                    "Rejected multi-row DELETE changed child table bytes");
        }
        {
            auto database = Database::open(root, "test");
            require(readFile(root / "tables/parents.table") == parentBytes &&
                        readFile(root / "tables/children.table") == childBytes,
                    "Rejected multi-row DELETE changed persisted table bytes");
            require(database.executeSql("SELECT * FROM parents;").rows.size() == 3 &&
                        database.executeSql("SELECT * FROM children;").rows.size() == 2,
                    "Rejected multi-row DELETE lost rows after reopening");
            rejectBatch(database, root, "parents", [&]
            {
                database.executeSql("DELETE FROM parents;");
            }, "FOREIGN KEY");

            require(database.executeSql("DELETE FROM children WHERE id = 10;").affectedRows == 1,
                    "Child deletion after reopening returned incorrect affectedRows");
            rejectBatch(database, root, "parents", [&]
            {
                database.executeSql("DELETE FROM parents WHERE id = 2;");
            }, "FOREIGN KEY");
            require(database.executeSql("DELETE FROM children;").affectedRows == 1,
                    "Deleting the remaining child returned incorrect affectedRows");
            require(database.executeSql("DELETE FROM parents;").affectedRows == 3,
                    "Removing all children did not allow the complete parent batch");
        }
        auto reopened = Database::open(root, "test");
        require(reopened.executeSql("SELECT * FROM parents;").rows.empty() &&
                    reopened.executeSql("SELECT * FROM children;").rows.empty(),
                "Successful deletes did not survive reopening");
        require(!std::filesystem::exists(root / "journal/journal.log"),
                "Successful deletes left an active journal");
    }

    void testNumericKeyEquality()
    {
        TestDirectory directory{"db-constraint-numeric"};
        const auto root = directory.path / "db";
        auto database = Database::create(root, "test");
        database.executeSql("CREATE TABLE decimals (amount DECIMAL UNIQUE);");
        database.executeSql("INSERT INTO decimals VALUES (1.00);");
        rejectBatch(database, root, "decimals", [&]
        {
            database.executeSql("INSERT INTO decimals VALUES (2.0), (2.00);");
        }, "UNIQUE");
        rejectBatch(database, root, "decimals", [&]
        {
            database.executeSql("INSERT INTO decimals VALUES (3.0), (1.000);");
        }, "UNIQUE");

        database.executeSql("CREATE TABLE floats (amount DOUBLE UNIQUE);");
        database.executeSql("INSERT INTO floats VALUES (1.0), (2.0);");
        rejectBatch(database, root, "floats", [&]
        {
            database.executeSql("INSERT INTO floats VALUES (3.0), (4.0), (3.0);");
        }, "UNIQUE");
        rejectBatch(database, root, "floats", [&]
        {
            database.executeSql("INSERT INTO floats VALUES (-0.0), (0.0);");
        }, "UNIQUE");
        database.executeSql("INSERT INTO floats VALUES (-0.0);");
        rejectBatch(database, root, "floats", [&]
        {
            database.executeSql("INSERT INTO floats VALUES (3.0), (0.0);");
        }, "UNIQUE");
    }

    void testNonSqlNumericKeys()
    {
        // SQL binding rejects NaN and normalizes decimal literals. Exercise
        // these typed values at the storage/validator boundary instead.
        TestDirectory directory{"db-constraint-numeric-storage"};
        const auto root = directory.path / "db";
        {
            auto database = Database::create(root, "test");
            database.executeSql("CREATE TABLE floats (amount DOUBLE UNIQUE);");
            database.executeSql("CREATE TABLE decimals (amount DECIMAL UNIQUE);");
        }
        auto engine = StorageEngine::open(root);
        const auto nan = std::numeric_limits<std::float64_t>::quiet_NaN();
        engine.beginStatement();
        engine.insertRows({.tableName = "floats", .rows = {
            Row{{nan}}, Row{{std::float64_t{1}}}, Row{{std::float64_t{2}}}, Row{{nan}}}});
        engine.insertRows({.tableName = "decimals", .rows = {Row{{DecimalValue{100, 2}}}}});
        engine.commitStatement();

        StorageConstraintLookup lookup{engine};
        ConstraintValidator validator{lookup};
        const auto floats = engine.getTableHeader("floats");
        require(validator.validateConstraint(floats, {
                    Row{{nan}}, Row{{std::float64_t{3}}}, Row{{nan}}}).valid,
                "NaN incorrectly conflicts with a stored or batch key");
        require(!validator.validateConstraint(floats, {
                    Row{{std::float64_t{3}}}, Row{{nan}}, Row{{std::float64_t{3}}}}).valid,
                "NaN hid a duplicate finite batch key");
        require(!validator.validateConstraint(floats, {
                    Row{{nan}}, Row{{std::float64_t{1}}}}).valid,
                "NaN hid a duplicate stored key");

        const auto decimals = engine.getTableHeader("decimals");
        require(!validator.validateConstraint(decimals, {
                    Row{{DecimalValue{20, 1}}}, Row{{DecimalValue{200, 2}}}}).valid,
                "Equivalent decimal batch keys with different scales were accepted");
        require(!validator.validateConstraint(decimals, {
                    Row{{DecimalValue{30, 1}}}, Row{{DecimalValue{1000, 3}}}}).valid,
                "Equivalent decimal stored key with a different scale was accepted");
    }
}

int main()
{
    try
    {
        testPrimaryKeysAndReopen();
        testCompositeAndIndependentKeys();
        testUniqueNulls();
        testInvalidSqlRows();
        testOtherConstraints();
        testForeignKeyDeleteRestrictions();
        testForeignKeyDeleteCompositeKeys();
        testForeignKeyDeleteNulls();
        testForeignKeyDeleteIndependentReferences();
        testForeignKeyDeleteBatchAndReopen();
        testNumericKeyEquality();
        testNonSqlNumericKeys();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
