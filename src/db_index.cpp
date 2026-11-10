#include "db_index.h"

Index::Index(const LinuxDirectory &indexDir,
             const std::filesystem::path &relIndexPath,
             LinuxFile::OpenMode mode,
             StatementRecovery &statementRecovery) : bufferManager_(indexDir,
                                                                    relIndexPath,
                                                                    mode,
                                                                    statementRecovery) {}

Index Index::open(const LinuxDirectory &indexDir, const std::filesystem::path &relPath, StatementRecovery &statementRecovery)
{
    Index index{indexDir, relPath, LinuxFile::OpenMode::OpenExisting, statementRecovery};
    index.validateHeaderPage();
    return index;
}

Index Index::create(const LinuxDirectory &indexDir,
                    const CreateIndexPayload &createIndexPayload,
                    StatementRecovery &statementRecovery)
{
    Index index{indexDir, createIndexPayload.indexName + ".index", LinuxFile::OpenMode::CreateNew, statementRecovery};
    index.initializeNewIndex(createIndexPayload);
    return index;
}

void Index::validateHeaderPage()
{
    bufferManager_.getIndexHeaderPage();
}

void Index::initializeNewIndex(const CreateIndexPayload &createIndexPayload)
{
    bufferManager_.initializeNewIndex(createIndexPayload);
    bufferManager_.flushAll();
    bufferManager_.sync();
}