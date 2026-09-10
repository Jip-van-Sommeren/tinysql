#include "db_buffer_manager.h"

#include "db_page_factory.h"
#include "db_read.h"
#include "db_row_validator.h"
#include "db_write.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
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

PageGuard::PageGuard(
    BufferManager &bufferManager, PageId pageId, Page &page) noexcept
    : bufferManager_(&bufferManager), pageId_(pageId), page_(&page)
{
}

PageGuard::PageGuard(PageGuard &&other) noexcept
    : bufferManager_(std::exchange(other.bufferManager_, nullptr)),
      pageId_(std::exchange(other.pageId_, 0)),
      page_(std::exchange(other.page_, nullptr))
{
}

PageGuard &PageGuard::operator=(PageGuard &&other) noexcept
{
    if (this != &other)
    {
        release();
        bufferManager_ = std::exchange(other.bufferManager_, nullptr);
        pageId_ = std::exchange(other.pageId_, 0);
        page_ = std::exchange(other.page_, nullptr);
    }
    return *this;
}

PageGuard::~PageGuard() noexcept
{
    release();
}

void PageGuard::release() noexcept
{
    if (bufferManager_)
    {
        bufferManager_->unpinPage(pageId_);
        bufferManager_ = nullptr;
        page_ = nullptr;
        pageId_ = 0;
    }
}

Page &PageGuard::page()
{
    assert(page_ != nullptr);
    return *page_;
}

const Page &PageGuard::page() const
{
    assert(page_ != nullptr);
    return *page_;
}

void PageGuard::markDirty()
{
    assert(bufferManager_ != nullptr);
    bufferManager_->markDirty(pageId_);
}

BufferManager::BufferManager(std::filesystem::path path)
    : path(std::move(path))
{
}

Page &BufferManager::fetchPage(
    PageId pageId,
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
            .page = std::move(decodedPage)});

    return inserted.first->second.page;
}

PageGuard BufferManager::getPage(PageId pageId, const PageReader &reader)
{
    Page &page = fetchPage(pageId, reader);
    pinPage(pageId);
    return PageGuard(*this, pageId, page);
}

PageGuard BufferManager::getHeaderPage()
{
    PageGuard header = getPage(0, decodeHeaderPage);
    if (header.page().header.pageType != PageType::HeaderPage)
    {
        throw std::runtime_error("Page 0 is not a table header page");
    }
    return header;
}

PageGuard BufferManager::getDataPage(PageId pageId)
{
    const PageGuard header = getHeaderPage();
    PageGuard data = getPage(
        pageId,
        [&header](const RawPage &rawPage)
        {
            return decodeDataPage(rawPage, header.as<HeaderPage>());
        });
    if (data.page().header.pageType != PageType::DataPage)
    {
        throw std::runtime_error("Page is not a data page");
    }
    return data;
}

void BufferManager::pinPage(PageId pageId)
{
    PageFrame &frame = pages.at(pageId);
    if (frame.pinCount == std::numeric_limits<std::uint32_t>::max())
    {
        throw std::overflow_error("Page pin count overflow");
    }
    ++frame.pinCount;
}

void BufferManager::unpinPage(PageId pageId) noexcept
{
    auto it = pages.find(pageId);
    if (it == pages.end() || it->second.pinCount == 0)
    {
        // Only a live guard can release a pin. A mismatch is an ownership bug.
        std::terminate();
    }
    --it->second.pinCount;
}

void BufferManager::markDirty(PageId pageId)
{
    pages.at(pageId).dirty = true;
}

void BufferManager::insertAllRows(const std::vector<Row> &rows)
{
    if (rows.empty())
    {
        return;
    }

    PageGuard header = getHeaderPage();
    HeaderPage &headerPage = header.as<HeaderPage>();

    if (headerPage.lastDataPageId == 0)
    {
        createDataPage(header);
    }

    PageId currentPageId = headerPage.firstDataPageId;
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

        PageGuard data = getDataPage(currentPageId);

        if (enoughSpaceForInsert(
                data.page(),
                encodedRowSize(headerPage, row)))
        {
            appendRowToExistingDataPage(
                header,
                data,
                row);
            ++rowIndex;
        }
        else if (data.page().nextPageId() != 0)
        {
            currentPageId = data.page().nextPageId();
        }
        else
        {
            currentPageId = createDataPage(header);
        }
    }
}

void BufferManager::flushPage(PageId pageId)
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
    for (const auto &[pageId, frame] : pages)
    {
        if (frame.dirty)
        {
            flushPage(pageId);
        }
    }
}

void BufferManager::setPage(const Page &page, PageId pageId)
{
    const auto existing = pages.find(pageId);
    if (existing != pages.end() && existing->second.pinCount != 0)
    {
        throw std::runtime_error("Cannot replace a pinned page");
    }

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

        return encodeDataPage(page.header, headerPage, dataPage);
    }

    throw std::runtime_error("Unsupported page type while encoding cached page");
}

void BufferManager::writePageToFile(
    PageId pageId,
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
    PageGuard &header,
    PageGuard &data,
    const Row &row)
{
    HeaderPage &headerPage = header.as<HeaderPage>();
    DataPage &dataPage = data.as<DataPage>();
    PageHeader &dataPageHeader = data.page().header;

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
    data.markDirty();
    header.markDirty();
}

PageId BufferManager::createDataPage(PageGuard &header)
{
    HeaderPage &headerPage = header.as<HeaderPage>();

    PageId previousPageId = headerPage.lastDataPageId;
    PageId newPageId = headerPage.nextUnusedPageId;

    if (newPageId == 0)
    {
        newPageId = previousPageId + 1;
    }

    if (previousPageId != 0)
    {
        PageGuard previous = getDataPage(previousPageId);
        previous.page().header.nextPageId = newPageId;
        previous.markDirty();
    }
    else
    {
        headerPage.firstDataPageId = newPageId;
    }

    Page newDataPage = makeEmptyDataPage(newPageId);

    headerPage.lastDataPageId = newPageId;
    headerPage.nextUnusedPageId = newPageId + 1;
    header.markDirty();
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
