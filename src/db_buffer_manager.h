#pragma once

#include "db_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

using PageId = std::uint32_t;
class PageGuard
{
PageGuard(const PageGuard&) = delete;
PageGuard& operator=(const PageGuard&) = delete;
PageGuard(PageGuard&& other) noexcept
    : bufferManager_(other.bufferManager_),
      pageId_(other.pageId_),
      page_(other.page_)
{
    other.bufferManager_ = nullptr;
    other.page_ = nullptr;
}
public:
    PageGuard(
        BufferManager& bufferManager,
        PageId pageId,
        Page* page)
        : bufferManager_(&bufferManager),
          pageId_(pageId),
          page_(page)
    {
    }

    ~PageGuard()
    {
        if (bufferManager_)
        {
            bufferManager_->unpinPage(pageId_);
        }
    }

    Page& page()
    {
        return *page_;
    }

    const Page& page() const
    {
        return *page_;
    }

private:
    BufferManager* bufferManager_;
    PageId pageId_;
    Page* page_;
};


class BufferManager
{
public:
    using PageReader = std::function<Page(const RawPage &)>;

    explicit BufferManager(std::filesystem::path path);

    Page &fetchPage(std::uint32_t pageId, const PageReader &reader);
    void markDirty(std::uint32_t pageId);
    void insertAllRows(const std::vector<Row> &rows, Page &headerPage);
    void flushPage(std::uint32_t pageId);
    void flushAll();
    void setPage(const Page &page, std::uint32_t pageId);

    void unpinPage(std::uint32_t pageId);
    void pinPage(std::uint32_t pageId);
    PageGuard getPage(PageId id,
    const PageReader &reader);

private:
    std::filesystem::path path;
    std::unordered_map<std::uint32_t, PageFrame> pages;

    RawPage encodeCachedPage(const Page &page);
    void writePageToFile(
        std::uint32_t pageId,
        const RawPage &pageData);
    void appendRowToExistingDataPage(
        Page &headerPage,
        Page &dataPage,
        const Row &row);
    std::uint32_t createDataPage(Page &headerPage);
    bool enoughSpaceForInsert(const Page &page, std::size_t rowSize) const;
};
