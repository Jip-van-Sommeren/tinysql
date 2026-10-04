#pragma once

#include "db_table.h"
#include "db_catalog.h"
#include "db_journal_file.h"
#include "db_read.h"

#include <filesystem>
#include <string>
#include <vector>

class StorageEngine final : public Catalog
{
public:
    static StorageEngine create(const std::filesystem::path &path);
    static StorageEngine open(const std::filesystem::path &path);

    bool tableExists(const std::string &name) const override;
    HeaderPage getTableHeader(const std::string &name) const override;

    Table createTable(
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns,
        const std::vector<Constraint> &constraints);
    Table openTable(const std::string &tableName);
    void syncTablesDirectory()
    {
        tablesDirectory_.sync();
    }

    void syncJournalDirectory()
    {
        journalDirectory_.sync();
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

    // const std::filesystem::path &getTablesPath() const;

private:
    std::filesystem::path dbPath_;
    LinuxDirectory dbRootDirectory_;
    LinuxDirectory tablesDirectory_;
    LinuxDirectory journalDirectory_;
    StatementRecovery statementRecovery_;

    explicit StorageEngine(
        const std::filesystem::path &path, LinuxDirectory lockedRoot);
    std::filesystem::path getTablePath(const std::string &name) const
    {
        return dbPath_ / "tables" / (name + ".table");
    }

    static void throwError(const char *operation, int error, const std::filesystem::path &path)
    {
        throw std::system_error(error, std::generic_category(),
                                std::string{operation} + " failed: " + path.string());
    }
};
