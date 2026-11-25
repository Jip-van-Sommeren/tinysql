#pragma once

#include "db_storage.h"
#include "db_buffer_manager.h"
#include <functional>

class Table;
class Index;

struct IndexPathEntry
{
    PageId parentPageId;
    std::size_t childIndex;
};

struct IndexBulkLoadOptions
{
    // Working-set target for run generation and merge heads; independent of row count.
    std::size_t memoryBudgetBytes = 4 * 1024 * 1024;
    std::size_t mergeFanIn = 16;
};

struct IndexBulkLoadStats
{
    std::uint64_t rows = 0;
    std::uint64_t initialRuns = 0;
    std::size_t mergePasses = 0;
    std::uint64_t leafPages = 0;
    std::uint64_t internalPages = 0;
};

class IndexCursor
{
public:
    IndexCursor(const IndexCursor &) = delete;
    IndexCursor &operator=(const IndexCursor &) = delete;
    IndexCursor(IndexCursor &&other) noexcept;
    IndexCursor &operator=(IndexCursor &&other) noexcept;

    std::optional<LeafIndexEntry> next();
    void seek(const Row &key);
    void seek(const Row &key, const RowId &targetRowId);

private:
    friend class Index;

    explicit IndexCursor(Index &index);

    Index *index_;
    PageId pageId_;
    std::size_t rowIndex_ = 0;
    std::optional<PageGuard> currentPage_;
    std::uint64_t remainingPages_ = 0;
    std::vector<IndexPathEntry> ancestorPages_;
};

class Index
{
public:
    explicit Index(const LinuxDirectory &indexDir, const std::filesystem::path &relIndexPath, LinuxFile::OpenMode mode, StatementRecovery &statementRecovery, std::optional<std::vector<Column>> schema);

    static Index create(
        const LinuxDirectory &indexDir,
        const CreateIndexPayload &createIndexPayload,
        StatementRecovery &statementRecovery);

    static Index open(const LinuxDirectory &indexDir,
                      const std::filesystem::path &relIndexPath,
                      StatementRecovery &statementRecovery);
    // Incremental insertion into a new or existing tree, in the active statement.
    void insertIndexes(std::vector<LeafIndexEntry> indexValues);
    void insertIndexes(std::vector<LocatedRow> rows);
    void deleteIndexes(const std::vector<LocatedRow> &rows);
    void truncate();

    // Builds a newly created, empty index in the caller's active statement.
    // The source table must stay unchanged for the duration of the build.
    IndexBulkLoadStats bulkLoad(Table &table, const IndexBulkLoadOptions &options = {});
    using EntrySource = std::function<std::optional<LeafIndexEntry>()>;
    IndexBulkLoadStats bulkLoad(EntrySource next, const IndexBulkLoadOptions &options = {});
    IndexCursor scan();
    IndexCursor seekLowerBound(const Row &key) &;

    std::string getTableName() { return tableName_; };
    std::string getIndexName() { return indexName_; };

    void renameTable(const std::string &newTableName);
    void renameColumn(const ColumnId colId, const std::string &newColumnName);
    // The index must not contain the dropped table column.
    void remapTableColumnIdsAfterDrop(ColumnId droppedId);

private:
    friend class IndexCursor;
    BufferManager bufferManager_;
    StatementRecovery &statementRecovery_;
    std::string tableName_;
    std::string indexName_;

    void insertIndex(const LeafIndexEntry &entry);

    void validateHeaderPage();

    void initializeNewIndex(
        const CreateIndexPayload &createIndexPayload);
};
