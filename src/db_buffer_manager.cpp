#include "db_buffer_manager.h"

#include "db_page_factory.h"
#include "db_read.h"
#include "db_row_validator.h"
#include "db_write.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    std::fstream openOrCreateFile(const std::filesystem::path &path)
    {
        if (!std::filesystem::exists(path))
        {
            std::ofstream createFile{path, std::ios::binary};
        }

        return std::fstream{
            path,
            std::ios::in | std::ios::out | std::ios::binary};
    }
}

BufferManager::BufferManager(std::filesystem::path path)
    : path(std::move(path))
{
}

Page &BufferManager::getPage(
    std::uint32_t pageId,
    const PageReader &reader)
{
    auto existing = pages.find(pageId);
    if (existing != pages.end())
    {
        return existing->second.page;
    }

    RawPage rawPage = readPageFromFile(path, pageId);
    Page decodedPage = reader(rawPage);

    auto inserted = pages.emplace(
        pageId,
        PageFrame{
            .pageId = pageId,
            .page = std::move(decodedPage),
            .dirty = false});

    return inserted.first->second.page;
}

void BufferManager::markDirty(std::uint32_t pageId)
{
    pages.at(pageId).dirty = true;
}

void BufferManager::insertAllRows(
    const std::vector<Row> &rows,
    Page &headerPageContainer)
{
    if (headerPageContainer.header.pageType != PageType::HeaderPage)
    {
        throw std::runtime_error("No header page was passed");
    }

    HeaderPage &headerPage =
        std::get<HeaderPage>(headerPageContainer.data);

    if (rows.empty())
    {
        return;
    }

    if (headerPage.lastDataPageId == 0)
    {
        createDataPage(headerPageContainer);
    }

    std::uint32_t currentPageId = headerPage.firstDataPageId;
    std::size_t rowIndex = 0;

    while (rowIndex < rows.size())
    {
        const Row &row = rows[rowIndex];
        RowValidationResult validation =
            validateRowAgainstSchema(headerPage.columns, row);

        if (!validation.valid)
        {
            throw std::runtime_error(validation.message);
        }

        Page &dataPage = getPage(
            currentPageId,
            [&headerPage](const RawPage &rawPage)
            {
                return decodeDataPage(rawPage, headerPage);
            });

        if (enoughSpaceForInsert(
                dataPage,
                encodedRowSize(headerPage, row)))
        {
            appendRowToExistingDataPage(
                headerPageContainer,
                dataPage,
                row);
            ++rowIndex;
        }
        else if (dataPage.header.nextPageId != 0)
        {
            currentPageId = dataPage.header.nextPageId;
        }
        else
        {
            currentPageId = createDataPage(headerPageContainer);
        }
    }
}

void BufferManager::flushPage(std::uint32_t pageId)
{
    PageFrame &frame = pages.at(pageId);

    if (!frame.dirty)
    {
        return;
    }

    RawPage encodedPage = encodeCachedPage(frame.page);
    writePageToFile(pageId, encodedPage);
    frame.dirty = false;
}

void BufferManager::flushAll()
{
    for (auto &[pageId, frame] : pages)
    {
        if (!frame.dirty)
        {
            continue;
        }

        RawPage encodedPage = encodeCachedPage(frame.page);
        writePageToFile(pageId, encodedPage);
        frame.dirty = false;
    }
}

void BufferManager::setPage(const Page &page, std::uint32_t pageId)
{
    pages.insert_or_assign(
        pageId,
        PageFrame{
            .pageId = pageId,
            .page = page,
            .dirty = true});
}

RawPage BufferManager::encodeCachedPage(const Page &page)
{
    if (page.header.pageType == PageType::HeaderPage)
    {
        return encodePage(page);
    }

    if (page.header.pageType == PageType::DataPage)
    {
        auto header = pages.find(0);
        if (header == pages.end())
        {
            throw std::runtime_error(
                "Cannot encode data page without cached header page");
        }

        const HeaderPage &headerPage =
            std::get<HeaderPage>(header->second.page.data);
        const DataPage &dataPage = std::get<DataPage>(page.data);

        std::vector<Row> rows;
        rows.reserve(dataPage.rows.size());

        for (const RowEntry &entry : dataPage.rows)
        {
            rows.push_back(entry.row);
        }

        return encodeDataPage(page.header, headerPage, rows);
    }

    throw std::runtime_error("Unsupported page type while encoding cached page");
}

void BufferManager::writePageToFile(
    std::uint32_t pageId,
    const RawPage &pageData)
{
    std::fstream file{openOrCreateFile(path)};
    if (!file)
    {
        std::cout << "Failed to open file\n";
        return;
    }

    file.seekp(static_cast<std::streamoff>(pageId) * PAGE_SIZE);
    file.write(
        reinterpret_cast<const char *>(pageData.data()),
        static_cast<std::streamsize>(pageData.size()));
}

void BufferManager::appendRowToExistingDataPage(
    Page &headerPageContainer,
    Page &dataPageContainer,
    const Row &row)
{
    HeaderPage &headerPage =
        std::get<HeaderPage>(headerPageContainer.data);
    DataPage &dataPage = std::get<DataPage>(dataPageContainer.data);

    PageHeader &headerPageHeader = headerPageContainer.header;
    PageHeader &dataPageHeader = dataPageContainer.header;

    if (dataPageHeader.pageType != PageType::DataPage)
    {
        throw std::runtime_error("Page is not a data page");
    }

    const std::size_t encodedSize = encodedRowSize(headerPage, row);
    if (encodedSize > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::runtime_error("Row is too large for uint16_t slot size");
    }

    const std::uint16_t rowSize = static_cast<std::uint16_t>(encodedSize);
    const std::size_t slotSize = encodedSlotSize();

    dataPageHeader.slotCount =
        static_cast<std::uint16_t>(dataPage.slots.size());

    bool needsNewSlot = true;
    for (std::size_t slotIndex = 0;
         slotIndex < dataPage.slots.size();
         ++slotIndex)
    {
        Slot &slot = dataPage.slots[slotIndex];

        if (slot.has(SlotFlag::Deleted) && slot.size >= rowSize)
        {
            slot.clear(SlotFlag::Deleted);
            slot.size = rowSize;

            dataPage.rows.push_back(RowEntry{
                .slotIndex = static_cast<std::uint16_t>(slotIndex),
                .row = row});

            needsNewSlot = false;
            break;
        }
    }

    if (needsNewSlot)
    {
        if (dataPageHeader.freeSpaceEnd < dataPageHeader.freeSpaceStart)
        {
            throw std::runtime_error("Invalid free space pointers");
        }

        std::size_t freeSpace =
            dataPageHeader.freeSpaceEnd - dataPageHeader.freeSpaceStart;
        std::size_t neededSpace = slotSize + rowSize;

        if (neededSpace > freeSpace)
        {
            throw std::runtime_error("Row does not fit in selected data page");
        }

        std::uint16_t rowOffset = dataPageHeader.freeSpaceStart;
        dataPage.slots.push_back(Slot{
            .offset = rowOffset,
            .size = rowSize,
            .flags = 0});
        dataPage.rows.push_back(RowEntry{
            .slotIndex = static_cast<std::uint16_t>(dataPage.slots.size() - 1),
            .row = row});

        dataPageHeader.freeSpaceStart = static_cast<std::uint16_t>(
            dataPageHeader.freeSpaceStart + rowSize);
        dataPageHeader.freeSpaceEnd = static_cast<std::uint16_t>(
            dataPageHeader.freeSpaceEnd - slotSize);
        dataPageHeader.slotCount =
            static_cast<std::uint16_t>(dataPage.slots.size());
    }

    ++headerPage.totalRowCount;
    markDirty(dataPageHeader.pageId);
    markDirty(headerPageHeader.pageId);
}

std::uint32_t BufferManager::createDataPage(Page &headerPageContainer)
{
    if (headerPageContainer.header.pageType != PageType::HeaderPage)
    {
        throw std::runtime_error("Incorrect page type passed");
    }

    HeaderPage &headerPage =
        std::get<HeaderPage>(headerPageContainer.data);
    PageHeader &headerPageHeader = headerPageContainer.header;

    std::uint32_t previousPageId = headerPage.lastDataPageId;
    std::uint32_t newPageId = headerPage.nextUnusedPageId;

    if (newPageId == 0)
    {
        newPageId = previousPageId + 1;
    }

    if (previousPageId != 0)
    {
        Page &previousPage = getPage(
            previousPageId,
            [&headerPage](const RawPage &rawPage)
            {
                return decodeDataPage(rawPage, headerPage);
            });
        previousPage.header.nextPageId = newPageId;
        markDirty(previousPageId);
    }
    else
    {
        headerPage.firstDataPageId = newPageId;
    }

    Page newDataPage = makeEmptyDataPage(newPageId);

    headerPage.lastDataPageId = newPageId;
    headerPage.nextUnusedPageId = newPageId + 1;
    markDirty(headerPageHeader.pageId);
    setPage(newDataPage, newPageId);

    return newPageId;
}

bool BufferManager::enoughSpaceForInsert(
    const Page &page,
    std::size_t rowSize) const
{
    if (page.header.pageType != PageType::DataPage)
    {
        throw std::runtime_error("Invalid page type for insert space check");
    }

    if (page.header.freeSpaceEnd < page.header.freeSpaceStart)
    {
        throw std::runtime_error("Invalid free space pointers");
    }

    const std::size_t slotSize = encodedSlotSize();
    std::size_t freeSpace =
        page.header.freeSpaceEnd - page.header.freeSpaceStart;

    if (freeSpace >= rowSize + slotSize)
    {
        return true;
    }

    const DataPage &dataPage = std::get<DataPage>(page.data);
    for (const Slot &slot : dataPage.slots)
    {
        if (slot.has(SlotFlag::Deleted) && slot.size >= rowSize)
        {
            return true;
        }
    }

    return false;
}
