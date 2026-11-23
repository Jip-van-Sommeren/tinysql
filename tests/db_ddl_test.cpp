#include "db_database.h"
#include "db_query_exception.h"
#include "db_test_directory.h"
#include "db_test_journal.h"
#include "db_write.h"

#include <iostream>
#include <map>
#include <string>

namespace
{
    using Path = std::filesystem::path;
    using Image = std::map<Path, std::vector<std::byte>>;

    void require(bool condition, const std::string &message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    Image files(const Path &root)
    {
        Image image;
        for (const auto &directory : {"tables", "indexes"})
            for (const auto &file : std::filesystem::directory_iterator(root / directory))
                image.emplace(file.path().lexically_relative(root), journal_test::readFile(file.path()));
        return image;
    }

    void reject(Database &db, const std::string &sql, QueryErrorCode code)
    {
        const auto result = db.tryExecuteSql(sql);
        require(!result && result.error().code == code, "Expected query rejection: " + sql);
    }

    void seed(const Path &root)
    {
        auto db = Database::create(root, "test");
        db.executeSql("CREATE TABLE parents (id INT PRIMARY KEY);");
        db.executeSql("CREATE TABLE others (id INT PRIMARY KEY);");
        db.executeSql("CREATE TABLE children (id INT PRIMARY KEY, parent_id INT REFERENCES parents(id), "
                      "second_parent INT REFERENCES parents(id), other_id INT REFERENCES others(id), code INT, "
                      "label TEXT DEFAULT 'kept', CHECK (code > 0));");
        db.executeSql("CREATE TABLE grandchildren (id INT REFERENCES children(id));");
        db.executeSql("CREATE TABLE independent (id INT);");
        db.executeSql("CREATE INDEX parent_index ON parents(id);");
        db.executeSql("CREATE UNIQUE INDEX child_code ON children(code);");
        db.executeSql("CREATE INDEX grandchild_index ON grandchildren(id);");
        db.executeSql("INSERT INTO parents VALUES (1);");
        db.executeSql("INSERT INTO others VALUES (1);");
        db.executeSql("INSERT INTO independent VALUES (1);");
        for (int id = 1; id <= 12; ++id)
        {
            db.executeSql("INSERT INTO children VALUES (" + std::to_string(id) +
                          ", 1, 1, 1, " + std::to_string(id) + ", '" + std::string(3000, 'x') + "');");
            db.executeSql("INSERT INTO grandchildren VALUES (" + std::to_string(id) + ");");
        }
    }

    void testSqlAndNames()
    {
        TestDirectory directory{"db-ddl-names"};
        const auto root = directory.path / "db";
        auto db = Database::create(root, "test");
        db.executeSql("CREATE TABLE a (id INT);");
        db.executeSql("CREATE TABLE b (id INT);");
        db.executeSql("CREATE INDEX by_a ON a(id);");
        db.executeSql("CREATE INDEX a ON b(id);");
        db.executeSql("INSERT INTO a VALUES (1);");
        db.executeSql("INSERT INTO b VALUES (2);");
        const auto before = files(root);
        for (const auto &sql : {"DROP TABLE a,;", "TRUNCATE TABLE a,;",
                 "DROP TABLE a nonsense;", "TRUNCATE TABLE a nonsense;", "DROP INDEX by_a nonsense;"})
            reject(db, sql, QueryErrorCode::Syntax);
        reject(db, "DROP TABLE a, missing;", QueryErrorCode::Validation);
        reject(db, "TRUNCATE TABLE a, missing;", QueryErrorCode::Validation);
        reject(db, "DROP INDEX missing;", QueryErrorCode::Validation);
        require(files(root) == before, "Rejected DDL modified storage");
        db.executeSql("TRUNCATE TABLE a, b, a;");
        require(db.executeSql("SELECT * FROM a;").rows.empty() &&
                db.executeSql("SELECT * FROM b;").rows.empty(), "Multiple-table TRUNCATE failed");
        db.executeSql("DROP TABLE a;");
        require(!std::filesystem::exists(root / "tables/a.table") &&
                !std::filesystem::exists(root / "indexes/by_a.index") &&
                std::filesystem::exists(root / "indexes/a.index"), "DROP used table names as index names");
        db.executeSql("DROP INDEX a;");
        require(std::filesystem::exists(root / "tables/b.table") &&
                !std::filesystem::exists(root / "indexes/a.index"), "DROP INDEX deleted the wrong object");
        db.executeSql("DROP TABLE b;");
    }

    void testDropCascade()
    {
        TestDirectory directory{"db-ddl-drop-cascade"};
        const auto root = directory.path / "db";
        seed(root);
        {
            auto db = Database::open(root, "test");
            const auto before = files(root);
            reject(db, "DROP TABLE independent, parents;", QueryErrorCode::Constraint);
            require(files(root) == before, "DROP modified an earlier target before checking FKs");
            db.executeSql("DROP TABLE parents CASCADE;");
            require(db.executeSql("SELECT * FROM children;").rows.size() == 12 &&
                    db.executeSql("SELECT * FROM grandchildren;").rows.size() == 12,
                    "DROP CASCADE deleted referencing tables or data");
            require(journal_test::readFile(root / "indexes/child_code.index") ==
                    before.at("indexes/child_code.index"), "DROP CASCADE changed the surviving index");
        }
        {
            auto db = Database::open(root, "test");
            db.executeSql("INSERT INTO children(id, parent_id, second_parent, other_id, code) "
                          "VALUES (20, 999, 999, 1, 20);");
            reject(db, "INSERT INTO children(id, other_id, code) VALUES (21, 999, 21);",
                   QueryErrorCode::Constraint);
            reject(db, "INSERT INTO children(id, code) VALUES (21, -1);", QueryErrorCode::Constraint);
            require(std::get<std::string>(db.executeSql("SELECT label FROM children WHERE id = 20;").rows[0].values[0])
                    == "kept", "DEFAULT was removed along with the FK");
            db.executeSql("DROP TABLE children, grandchildren;");
        }
        require(!std::filesystem::exists(root / "indexes/child_code.index") &&
                !std::filesystem::exists(root / "indexes/grandchild_index.index"), "DROP left orphan indexes");
    }

    void testTruncateCascade()
    {
        TestDirectory directory{"db-ddl-truncate"};
        const auto root = directory.path / "db";
        seed(root);
        {
            auto db = Database::open(root, "test");
            const auto before = files(root);
            reject(db, "TRUNCATE TABLE independent, parents;", QueryErrorCode::Constraint);
            require(files(root) == before, "Rejected TRUNCATE changed an earlier target");
            db.executeSql("TRUNCATE TABLE parents CASCADE;");
            for (const auto &name : {"parents", "children", "grandchildren"})
            {
                require(db.executeSql("SELECT * FROM " + std::string(name) + ";").rows.empty(),
                        "TRUNCATE missed a transitive reference");
                require(std::filesystem::file_size(root / "tables" / (std::string(name) + ".table")) == 2 * PAGE_SIZE,
                        "TRUNCATE retained data pages");
            }
            for (const auto &name : {"parent_index", "child_code", "grandchild_index"})
                require(std::filesystem::exists(root / "indexes" / (std::string(name) + ".index")),
                        "TRUNCATE deleted an index definition");
        }
        {
            auto db = Database::open(root, "test");
            db.executeSql("INSERT INTO parents VALUES (1);");
            db.executeSql("INSERT INTO children(id, parent_id, other_id, code) VALUES (1, 1, 1, 1);");
            reject(db, "INSERT INTO children(id, parent_id, code) VALUES (2, 1, 1);", QueryErrorCode::Constraint);
            reject(db, "INSERT INTO children(id, parent_id, code) VALUES (2, 99, 2);", QueryErrorCode::Constraint);
            db.executeSql("INSERT INTO grandchildren VALUES (1);");
            db.executeSql("TRUNCATE TABLE parents, children, grandchildren;");
            require(db.executeSql("SELECT * FROM independent;").rows.size() == 1,
                    "TRUNCATE changed an unrelated table");
        }
    }

    enum class Operation { DropTable, DropIndex, Truncate };

    void mutate(StorageEngine &engine, Operation op)
    {
        switch (op)
        {
        case Operation::DropTable: engine.dropTable({{"parents"}, true}); break;
        case Operation::DropIndex: engine.dropIndex({"child_code"}); break;
        case Operation::Truncate: engine.truncateTable({{"parents"}, true}); break;
        }
    }

    void testRecovery()
    {
        for (const auto op : {Operation::DropTable, Operation::DropIndex, Operation::Truncate})
        {
            for (const bool reopen : {false, true})
            {
                TestDirectory directory{"db-ddl-recovery"};
                const auto root = directory.path / "db";
                seed(root);
                const auto before = files(root);
                {
                    auto engine = StorageEngine::open(root);
                    engine.beginStatement();
                    mutate(engine, op);
                    require(files(root) != before, "Recovery fixture made no changes");
                    if (!reopen) engine.rollbackStatement();
                    // Otherwise leave a durable journal for startup recovery.
                }
                {
                    auto db = Database::open(root, "test");
                    require(files(root) == before, "DDL rollback did not restore exact table/index bytes");
                    require(db.executeSql("SELECT * FROM children;").rows.size() == 12,
                            "DDL rollback lost rows");
                }
                require(!std::filesystem::exists(root / "journal/journal.log"), "Recovery retained its journal");
            }
        }
    }

    void testCyclesAndHandles()
    {
        TestDirectory directory{"db-ddl-cycles"};
        const auto root = directory.path / "db";
        {
            auto db = Database::create(root, "test");
            db.executeSql("CREATE TABLE a (id INT PRIMARY KEY);");
            db.executeSql("CREATE TABLE b (id INT PRIMARY KEY REFERENCES a(id));");
            db.executeSql("INSERT INTO a VALUES (1);");
            db.executeSql("INSERT INTO b VALUES (1);");
        }
        // Seed a cycle and self-reference; CREATE cannot bind forward references.
        {
            PageFile file{LinuxDirectory{root / "tables"}, "a.table"};
            auto page = decodeHeaderPage(file.readPage(HeaderPageId));
            auto &header = std::get<HeaderPage>(page.data);
            header.constraints.push_back(BoundForeignKeyConstraintExpr{"a_b", {0}, "b", {0}});
            header.constraints.push_back(BoundForeignKeyConstraintExpr{"a_a", {0}, "a", {0}});
            file.writePage(HeaderPageId, encodeHeaderPage(page.header, header));
            file.sync();
        }
        {
            auto engine = StorageEngine::open(root);
            auto oldHandle = engine.openTable("a");
            engine.beginStatement();
            engine.truncateTable({{"a"}, true});
            engine.commitStatement();
            bool rejected = false;
            try { (void)oldHandle.scan(); }
            catch (const std::runtime_error &) { rejected = true; }
            require(rejected, "TRUNCATE allowed use of a stale table handle");
        }
        {
            auto db = Database::open(root, "test");
            require(db.executeSql("SELECT * FROM a;").rows.empty() && db.executeSql("SELECT * FROM b;").rows.empty(),
                    "TRUNCATE failed to resolve an FK cycle");
            db.executeSql("DROP TABLE a, b;");
        }
    }
}

int main()
{
    try
    {
        testSqlAndNames();
        testDropCascade();
        testTruncateCascade();
        testRecovery();
        testCyclesAndHandles();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
