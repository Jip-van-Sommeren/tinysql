#pragma once

#include "db_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <variant>
#include <vector>

using PageId = std::uint32_t;

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

    Page &page();
    const Page &page() const;
    void markDirty();

    template <typename T>
    T &as()
    {
        return std::get<T>(page().data);
    }

    template <typename T>
    const T &as() const
    {
        return std::get<T>(page().data);
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

    explicit BufferManager(std::filesystem::path path);
    BufferManager(const BufferManager &) = delete;
    BufferManager &operator=(const BufferManager &) = delete;
    BufferManager(BufferManager &&) = default;
    BufferManager &operator=(BufferManager &&) = default;

    PageGuard getPage(PageId pageId, const PageReader &reader);
    PageGuard getHeaderPage();
    PageGuard getDataPage(PageId pageId);
    void insertAllRows(const std::vector<Row> &rows);
    void flushPage(PageId pageId);
    void flushAll();
    void setPage(const Page &page, PageId pageId);

private:
    friend class PageGuard;

    std::filesystem::path path;
    std::unordered_map<PageId, PageFrame> pages;

    Page &fetchPage(PageId pageId, const PageReader &reader);
    void pinPage(PageId pageId);
    void unpinPage(PageId pageId) noexcept;
    void markDirty(PageId pageId);

    RawPage encodeCachedPage(const Page &page);
    void writePageToFile(
        PageId pageId,
        const RawPage &pageData);
    void appendRowToExistingDataPage(
        PageGuard &headerPage,
        PageGuard &dataPage,
        const Row &row);
    PageId createDataPage(PageGuard &headerPage);
    bool enoughSpaceForInsert(const Page &page, std::size_t rowSize) const;
};
