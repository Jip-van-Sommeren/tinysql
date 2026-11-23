#pragma once

#include "db_table.h"
#include "db_index.h"
#include "db_catalog.h"
#include "db_journal_file.h"
#include "db_read.h"
#include "db_query_result.h"

#include <filesystem>
#include <string>
#include <set>
#include <vector>

struct BoundInsert;
struct BoundDelete;
struct BoundUpdate;
struct BoundCreateTable;
struct BoundCreateIndex;
struct BoundDropTable;
struct BoundTruncateTable;
struct BoundDropIndex;

class StorageEngine final : public Catalog
{
public:
    static StorageEngine create(const std::filesystem::path &path);
    static StorageEngine open(const std::filesystem::path &path);

    bool tableExists(const std::string &name) const override;
    bool indexExists(const std::string &name) const override;
    HeaderPage getTableHeader(const std::string &name) const override;
    std::vector<Column> getTableSchema(const std::string &tableName) const;

    Table createTable(
        const BoundCreateTable &createTable);
    Table openTable(const std::string &tableName);
    void requireReadable() const { statementRecovery_.requireReadable(); }
    void syncTablesDirectory()
    {
        syncDirectory(tablesDirectory_);
    }

    void syncJournalDirectory()
    {
        syncDirectory(journalDirectory_);
    }
    void beginStatement()
    {
        statementRecovery_.begin();
    }

    void commitStatement()
    {
        statementRecovery_.commit();
    }

    void rollbackStatement()
    {
        statementRecovery_.rollback();
    }
    QueryResult insertRows(const BoundInsert &insert);
    QueryResult deleteRows(const BoundDelete &del);
    QueryResult updateRows(const BoundUpdate &update);

    QueryResult dropTable(const BoundDropTable &dropTable);
    QueryResult dropIndex(const BoundDropIndex &dropIndex);
    QueryResult truncateTable(const BoundTruncateTable &truncateTable);

    Index createIndex(const BoundCreateIndex &createIndex, const IndexBulkLoadOptions &options = {});
    Index openIndex(const std::string &indexName);
    Index openIndex(const std::filesystem::path &fileName);
    std::vector<Index> indexesForTable(const std::string &indexName);
    std::vector<std::string> indexNamesForTable(const std::string &tableName);

    void createIndexForPkAndUniqueConstraints(const BoundCreateTable &createTable);

    // const std::filesystem::path &getTablesPath() const;

private:
    enum class TableOperation
    {
        Drop,
        Truncate
    };
    struct TableDdlPlan
    {
        std::set<std::string> targets;
        std::set<std::string> referencingTables;
    };
    TableDdlPlan planTableDdl(const std::vector<std::string> &requested,
                              bool cascade, TableOperation operation);

    void syncDirectory(LinuxDirectory &directory)
    {
        requireReadable();
        try
        {
            directory.sync();
        }
        catch (...)
        {
            statementRecovery_.statementFailed();
            throw;
        }
    }

    std::filesystem::path dbPath_;
    LinuxDirectory dbRootDirectory_;
    LinuxDirectory tablesDirectory_;
    LinuxDirectory indexDirectory_;
    LinuxDirectory journalDirectory_;
    StatementRecovery statementRecovery_;

    explicit StorageEngine(
        const std::filesystem::path &path, LinuxDirectory lockedRoot, const uuid_t &dbId);
    std::filesystem::path getTablePath(const std::string &name) const
    {
        return dbPath_ / "tables" / (name + ".table");
    }

    std::filesystem::path getIndexPath(const std::string &name) const
    {
        return dbPath_ / "indexes" / (name + ".index");
    }

    static void throwError(const char *operation, int error, const std::filesystem::path &path)
    {
        throw std::system_error(error, std::generic_category(),
                                std::string{operation} + " failed: " + path.string());
    }
    static void createMetaFile(const std::filesystem::path &dbPath, const uuid_t &dbId);
};
