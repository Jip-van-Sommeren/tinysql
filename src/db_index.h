#pragma once

#include <string>
#include <vector>
#include "db_linux_file.h"
#include "db_statement_recovery.h"
#include "db_buffer_manager.h"
#include "db_query_validator.h"

struct CreateIndexPayload
{
    std::string indexName;
    std::string tableName;
    std::string magic;
    std::vector<ColumnId> columns;
};

class Index
{
public:
    explicit Index(const LinuxDirectory &indexDir, const std::filesystem::path &relIndexPath, LinuxFile::OpenMode mode, StatementRecovery &statementRecovery);
    void createIndex(const BoundCreateIndex &createIndex);

    static Index create(
        const LinuxDirectory &indexDir,
        const CreateIndexPayload &createIndexPayload,

        StatementRecovery &statementRecovery);

    static Index open(const LinuxDirectory &indexDir,
                      const std::filesystem::path &relIndexPath,
                      StatementRecovery &statementRecovery);

private:
    BufferManager bufferManager_;

    void validateHeaderPage();

    void initializeNewIndex(
        const CreateIndexPayload &createIndexPayload);
};
