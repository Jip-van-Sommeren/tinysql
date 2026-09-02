#pragma once

#include "db_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

class BufferManager
{
public:
    using PageReader = std::function<Page(const RawPage &)>;

    explicit BufferManager(std::filesystem::path path);

    Page &getPage(std::uint32_t pageId, const PageReader &reader);
    void markDirty(std::uint32_t pageId);
    void insertAllRows(const std::vector<Row> &rows, Page &headerPage);
    void flushPage(std::uint32_t pageId);
    void flushAll();
    void setPage(const Page &page, std::uint32_t pageId);

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
