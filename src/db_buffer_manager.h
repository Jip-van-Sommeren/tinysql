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
        LinuxFile::OpenMode mode, StatementRecovery &statementRecovery);
    BufferManager(const BufferManager &) = delete;
    BufferManager &operator=(const BufferManager &) = delete;
    BufferManager(BufferManager &&) = default;
    BufferManager &operator=(BufferManager &&) = default;

    PageGuard getPage(PageId pageId, const PageReader &reader);
    PageGuard getHeaderPage();
    PageGuard getDataPage(PageId pageId);

    void initializeNewTable(const std::string &tableName,
                            const std::string &magic,
                            const std::vector<Column> &columns,
                            const std::vector<BoundConstraint> &constraints);

    void insertRows(const std::vector<Row> &rows);
    void insertRow(const Row &row);
    void updateRows(const std::vector<LocatedRow> &rows);
    std::uint64_t deleteRow(const RowId &rowId);

    template <std::ranges::input_range Rows>
    std::uint64_t deleteRows(Rows &&rowIds)
    {
        requireReadable();
        std::uint64_t deletedCount = 0;
        for (const RowId &rowId : rowIds)
            deletedCount += deleteRow(rowId);
        return deletedCount;
    }
    void flushPage(PageId pageId);
    void flushAll();
    void sync();
    void requireReadable() const;

private:
    friend class PageGuard;

    // Instantiated only by the concrete insertion/update methods in the .cpp.
    template <std::ranges::input_range Rows>
    void insertRowsImpl(Rows &&rows);

    PageFile pageFile_;
    StatementRecovery &statementRecovery_;
    std::filesystem::path relativeTablePath_;
    const std::uint64_t cacheEpoch_;
    std::shared_ptr<StatementRecovery::Participant> participant_;

    std::unordered_map<PageId, PageFrame> pages;

    void prepareForWrite(PageId pageId);

    void setPage(const Page &page, PageId pageId);

    Page &fetchPage(PageId pageId, const PageReader &reader);
    void pinPage(PageId pageId);
    void unpinPage(PageId pageId) noexcept;
    void markDirty(PageId pageId);

    RawPage encodeCachedPage(const Page &page);

    void appendRowToExistingDataPage(
        PageGuard &headerPage,
        PageGuard &dataPage,
        const Row &row);
    PageId createDataPage(PageGuard &headerPage);
    bool enoughSpaceForInsert(const Page &page, std::size_t rowSize) const;
};
