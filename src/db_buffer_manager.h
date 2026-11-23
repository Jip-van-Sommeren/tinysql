#pragma once

#include "db_storage.h"
#include "db_page_file.h"
#include "db_journal_file.h"
#include "db_statement_recovery.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <ranges>
#include <unordered_map>
#include <variant>
#include <vector>

#include <concepts>

template <typename T>
concept IndexEntryType =
    std::same_as<T, LeafIndexEntry> ||
    std::same_as<T, InternalIndexEntry>;

template <typename T>
concept PagePayload = std::same_as<T, DataPage> || std::same_as<T, IndexPage>;
class BufferManager;

// Owns one pin on a cached page. The manager must outlive the guard and
// must not be moved while the guard is alive.
class PageGuard
{
public:
    PageGuard(const PageGuard &) = delete;
    PageGuard &operator=(const PageGuard &) = delete;
    PageGuard(PageGuard &&other) noexcept;
    PageGuard &operator=(PageGuard &&other) noexcept;
    ~PageGuard() noexcept;

    const Page &page() const;
    void markDirty();

    template <typename T>
    const T &as() const
    {
        return std::get<T>(page().data);
    }
    Page &writePage();

    template <typename T>
    T &write()
    {
        // Check the variant type before changing recovery/dirty state.
        (void)as<T>();

        return std::get<T>(writePage().data);
    }

private:
    friend class BufferManager;

    PageGuard(BufferManager &bufferManager, PageId pageId, Page &page) noexcept;
    void release() noexcept;

    BufferManager *bufferManager_;
    PageId pageId_;
    Page *page_;
};

class BufferManager
{
public:
    using PageReader = std::function<Page(const RawPage &)>;

    explicit BufferManager(
        const LinuxDirectory &tablesDir, std::filesystem::path relTablePath,
        LinuxFile::OpenMode mode, StatementRecovery &statementRecovery, std::optional<std::vector<Column>> schema = std::nullopt, bool indexFile = false);
    BufferManager(const BufferManager &) = delete;
    BufferManager &operator=(const BufferManager &) = delete;
    BufferManager(BufferManager &&) = default;
    BufferManager &operator=(BufferManager &&) = default;

    PageGuard getPage(PageId pageId, const PageReader &reader);
    PageGuard getHeaderPage();

    PageGuard getIndexHeaderPage();

    template <PagePayload T>
    PageGuard getPage(PageId pageId);

    void truncateTable();
    void truncateIndex();

    void initializeNewTable(const CreateTablePayload &createTablePayload);
    void initializeNewIndex(const CreateIndexPayload &CreateIndexPayload);

    std::vector<LocatedRow> insertRows(const std::vector<Row> &rows);
    std::vector<LocatedRow> insertRow(const Row &row);
    std::vector<LocatedRow> updateRows(const std::vector<LocatedRow> &rows);
    std::uint64_t deleteRow(const RowId &rowId);

    std::uint64_t deleteIndex(const PageId pageId, const size_t index);

    template <std::ranges::input_range Rows>
    std::uint64_t deleteRows(Rows &&rowIds)
    {
        requireReadable();
        std::uint64_t deletedCount = 0;
        for (const RowId &rowId : rowIds)
            deletedCount += deleteRow(rowId);
        return deletedCount;
    }

    // Bulk pages are written immediately and released, keeping construction bounded.
    void requireNewIndex() const;
    void writeBulkIndexPage(const Page &page);
    std::uint64_t pageCount() const { return pageFile_.size() / PAGE_SIZE; }
    void flushPage(PageId pageId);
    void flushAll();
    void sync();
    void requireReadable() const;
    const RowSchema &getSchema() const { return schema_; }

    // Mutate one node. A split returns the separator for its new right sibling.
    std::optional<InternalIndexEntry> insertIntoLeaf(PageGuard &currentPage, std::size_t index, const LeafIndexEntry &entry);

    void createNewRoot(PageId leftPageId, std::uint16_t level, const InternalIndexEntry &internal);

    std::optional<InternalIndexEntry> insertIntoInternal(PageGuard &parentPage, std::size_t childIndex, const InternalIndexEntry &internal);

private:
    friend class PageGuard;
    void resetToEmpty();

    // Instantiated only by the concrete insertion/update methods in the .cpp.
    RowSchema schema_;
    bool indexFile_;
    PageFile pageFile_;
    StatementRecovery &statementRecovery_;
    std::filesystem::path relativeTablePath_;
    const std::uint64_t cacheEpoch_;
    std::shared_ptr<StatementRecovery::Participant> participant_;

    std::unordered_map<PageId, PageFrame> pages;

    template <std::ranges::input_range Rows>
    std::vector<LocatedRow> insertRowsImpl(Rows &&rows);

    void prepareForWrite(PageId pageId);

    void setPage(const Page &page, PageId pageId);

    PageGuard createIndexPage(std::uint16_t level, PageId firstChildPageId = 0);
    void replaceIndexPage(PageGuard &page, IndexPage contents, PageId nextPageId);

    void evictCleanPages();
    Page &fetchPage(PageId pageId, const PageReader &reader);
    void pinPage(PageId pageId);
    void unpinPage(PageId pageId) noexcept;
    void markDirty(PageId pageId);

    RawPage encodeCachedPage(const Page &page);

    LocatedRow appendRowToExistingDataPage(
        PageGuard &headerPage,
        PageGuard &dataPage,
        const Row &row);

    template <PagePayload T>
    PageId createPage(PageGuard &header);

    template <PagePayload T>
    bool enoughSpaceForInsert(
        const Page &page,
        std::size_t rowSize) const;
};
