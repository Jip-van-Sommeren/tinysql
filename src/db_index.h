#pragma once

#include "db_storage.h"
#include "db_buffer_manager.h"

class Index
{
public:
    explicit Index(const LinuxDirectory &indexDir, const std::filesystem::path &relIndexPath, LinuxFile::OpenMode mode, StatementRecovery &statementRecovery);

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
