#include "db_buffer_manager.h"

#include "db_page_factory.h"
#include "db_read.h"
#include "db_row_validator.h"
#include "db_write.h"
#include "db_index_codec.h"

#include <algorithm>
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
#include <ranges>
#include <span>

namespace
{
    PageFile openPageFile(const LinuxDirectory &tablesDir,
                          const std::filesystem::path &relativePath,
                          LinuxFile::OpenMode mode,
                          StatementRecovery &recovery, bool indexFile)
    {
        recovery.requireReadable();
        if (mode == LinuxFile::OpenMode::CreateNew)
        {
            // Persist nonexistence before open(O_CREAT) can change the directory.
            recovery.captureFileOnce(indexFile ? std::filesystem::path{"indexes"} / relativePath : relativePath, true);
            recovery.ensureDurable();
        }
        try
        {
            return PageFile{tablesDir, relativePath, mode};
        }
        catch (...)
        {
            if (mode == LinuxFile::OpenMode::CreateNew)
                recovery.statementFailed();
            throw;
        }
    }
    const std::string pageTypeToString(PageType type)
    {
        switch (type)
        {
        case PageType::DataPage:
            return "DataPage";
        case PageType::IndexPage:
            return "IndexPage";
        case PageType::OverflowPage:
            return "OverflowPage";
        case PageType::FreePage:
            return "FreePage";
        case PageType::HeaderPage:
            return "HeaderPage";
        default:
            throw std::runtime_error("Unknown page type");
        }
    };
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

Page &PageGuard::writePage()
{
    assert(page_ != nullptr);
    assert(bufferManager_ != nullptr);

    bufferManager_->markDirty(pageId_);

    return *page_;
}

const Page &PageGuard::page() const
{
    assert(page_ != nullptr);
    bufferManager_->requireReadable();
    return *page_;
}

void PageGuard::markDirty()
{
    assert(bufferManager_ != nullptr);
    bufferManager_->markDirty(pageId_);
}

BufferManager::BufferManager(const LinuxDirectory &tablesDir,
                             std::filesystem::path relTablePath,
                             LinuxFile::OpenMode mode,
                             StatementRecovery &statementRecovery, std::optional<std::vector<Column>> schema, bool indexFile)
    : indexFile_(indexFile),
      pageFile_(openPageFile(tablesDir, relTablePath, mode, statementRecovery, indexFile)),
      statementRecovery_(statementRecovery),
      relativeTablePath_(indexFile ? std::filesystem::path{"indexes"} / relTablePath : relTablePath),
      cacheEpoch_(statementRecovery.cacheEpoch()),
      participant_(std::make_shared<StatementRecovery::Participant>())
{
    participant_->relativePath = relativeTablePath_;
    if (mode == LinuxFile::OpenMode::CreateNew)
    {
        statementRecovery_.trackParticipant(participant_);
        participant_->needsSync = true;
    }

    if (schema)
    {
        schema_ = RowSchema{.columns = schema.value()};
    }
    else if (indexFile_ && mode != LinuxFile::OpenMode::CreateNew)
    {
        schema_.columns = getIndexHeaderPage().as<IndexHeaderPage>().columns;
    }
}

void BufferManager::requireReadable() const
{
    statementRecovery_.requireReadable();
    if (cacheEpoch_ != statementRecovery_.cacheEpoch())
        throw std::runtime_error("Table handle was invalidated by rollback; reopen the table");
}

void BufferManager::prepareForWrite(PageId pageId)
{
    requireReadable();
    if (!statementRecovery_.isActive())
        throw std::logic_error("Cannot modify a page without an active statement");
    try
    {
        statementRecovery_.captureFileOnce(relativeTablePath_);
        if (statementRecovery_.hasCapturedPage(relativeTablePath_, pageId))
            return;

        if (std::uint64_t{pageId} * PAGE_SIZE < statementRecovery_.getOriginalFileSize(relativeTablePath_))
        {
            const RawPage originalPage = pageFile_.readPage(pageId);
            statementRecovery_.capturePageOnce(PageBeforeImage{
                .relativeFilePath = relativeTablePath_,
                .pageId = pageId,
                .originalPage = originalPage,
            });
        }
    }
    catch (...)
    {
        // Failure to read an original page is also a capture failure.
        statementRecovery_.statementFailed();
        throw;
    }
}

void BufferManager::evictCleanPages()
{
    // Pinned pages and dirty pages remain stable; metadata stays cached.
    constexpr std::size_t CacheLimit = 64;
    if (pages.size() < CacheLimit)
        return;
    for (auto it = pages.begin(); it != pages.end() && pages.size() >= CacheLimit;)
    {
        if (it->first != 0 && it->second.pinCount == 0 && !it->second.dirty)
            it = pages.erase(it);
        else
            ++it;
    }
}

Page &BufferManager::fetchPage(
    PageId pageId,
    const PageReader &reader)
{
    requireReadable();
    auto existing = pages.find(pageId);
    if (existing != pages.end())
    {
        return existing->second.page;
    }

    RawPage rawPage = pageFile_.readPage(pageId);
    Page decodedPage = reader(rawPage);

    evictCleanPages();
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
    PageGuard header = getPage(PageId{0}, decodeHeaderPage);
    if (header.page().header.pageType != PageType::HeaderPage)
    {
        throw std::runtime_error("Page 0 is not a header page");
    }
    if (schema_.columns.empty())
        schema_.columns = header.as<HeaderPage>().columns;
    return header;
}

PageGuard BufferManager::getIndexHeaderPage()
{
    PageGuard header = getPage(PageId{0}, decodeIndexHeaderPage);
    if (header.page().header.pageType != PageType::IndexHeaderPage)
    {
        throw std::runtime_error("Page 0 is not a header page");
    }
    return header;
}

template <PagePayload T>
PageGuard BufferManager::getPage(PageId pageId)
{
    PageGuard data = [&]()
    {
        if constexpr (std::same_as<T, DataPage>)
        {
            const PageGuard header = getHeaderPage();
            return getPage(pageId, [&header](const RawPage &raw)
                           { return decodeDataPage(raw, header.as<HeaderPage>()); });
        }
        else
        {
            return getPage(pageId, [this](const RawPage &raw)
                           { return decodeIndexPage(raw, schema_); });
        }
    }();
    if (data.page().header.pageType != T::type())
    {
        throw std::runtime_error("Page " + std::to_string(pageId) + " is not a " +
                                 pageTypeToString(T::type()));
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
    prepareForWrite(pageId);
    auto &frame = pages.at(pageId);
    frame.coverage = statementRecovery_.requiredCoverageForPage(relativeTablePath_, pageId);
    statementRecovery_.trackParticipant(participant_);
    participant_->dirtyPages.insert(pageId);
    frame.dirty = true;
}

std::uint64_t BufferManager::deleteRow(const RowId &rowId)
{
    requireReadable();
    PageGuard data = getPage<DataPage>(rowId.pageId);
    const DataPage &stored = data.as<DataPage>();
    if (stored.slots.at(rowId.slotIndex).has(SlotFlag::Deleted))
        return 0;
    const auto entry = std::ranges::find(stored.rows, rowId.slotIndex, &RowEntry::slotIndex);
    if (entry == stored.rows.end())
        throw std::runtime_error("Active slot has no row");

    PageGuard header = getHeaderPage();
    if (header.as<HeaderPage>().totalRowCount == 0)
        throw std::runtime_error("Deleted row count exceeds table row count");

    // Capture both before-images before changing either page. Vector positions
    // shift after erase; the slot index remains the row's address.
    HeaderPage &headerPage = header.write<HeaderPage>();
    DataPage &dataPage = data.write<DataPage>();
    dataPage.rows.erase(entry);
    dataPage.slots.at(rowId.slotIndex).set(SlotFlag::Deleted);
    --headerPage.totalRowCount;
    data.markDirty();
    header.markDirty();
    return 1;
}

std::uint64_t BufferManager::deleteIndex(const PageId pageId, const size_t index)
{
    requireReadable();
    PageGuard data = getPage<IndexPage>(pageId);
    const IndexPage &stored = data.as<IndexPage>();
    if (stored.slots.at(index).has(SlotFlag::Deleted))
        return 0;

    // Capture both before-images before changing either page. Vector positions
    // shift after erase; the slot index remains the row's address.
    IndexPage &dataPage = data.write<IndexPage>();
    auto &mutableEntries =
        std::get<std::vector<LeafIndexEntry>>(dataPage.entries);
    mutableEntries.erase(mutableEntries.begin() + index);
    dataPage.slots.at(index).set(SlotFlag::Deleted);
    // --headerPage.totalRowCount;
    data.markDirty();
    return 1;
}

void BufferManager::updateRows(const std::vector<LocatedRow> &rows)
{
    auto delRowIds = rows | std::views::transform(
                                [](const LocatedRow &located) -> const RowId &
                                {
                                    return located.id;
                                });
    deleteRows(delRowIds);

    auto replacements = rows | std::views::transform(
                                   [](const LocatedRow &located) -> const Row &
                                   {
                                       return located.row;
                                   });

    insertRowsImpl(replacements);
}

std::vector<LocatedRow> BufferManager::insertRow(const Row &row)
{
    return insertRowsImpl(std::span<const Row>{&row, 1});
}

std::vector<LocatedRow> BufferManager::insertRows(const std::vector<Row> &rows)
{
    return insertRowsImpl(rows);
}

template <std::ranges::input_range Rows>
std::vector<LocatedRow> BufferManager::insertRowsImpl(Rows &&rows)
{
    requireReadable();
    auto current = std::ranges::begin(rows);
    const auto end = std::ranges::end(rows);
    if (current == end)
        return {};

    PageGuard header = getHeaderPage();
    const HeaderPage &headerPage = header.as<HeaderPage>();
    PageId currentPageId = headerPage.firstDataPageId;
    std::vector<LocatedRow> LocatedRows{};
    LocatedRows.reserve(rows.size());
    for (; current != end; ++current)
    {
        const Row &row = *current;
        const auto validation = validateRowAgainstSchema(headerPage.columns, row);
        if (!validation.valid)
            throw std::runtime_error(validation.message);

        const auto rowSize = encodedRowSize(headerPage, row);
        const auto maxRowSize = PAGE_SIZE - PageHeaderLayout::Size - encodedSlotSize();
        if (rowSize > maxRowSize)
            throw std::runtime_error("Row is too large to fit on a page");

        if (headerPage.lastDataPageId == 0)
            currentPageId = createPage<DataPage>(header);

        while (true)
        {
            PageGuard data = getPage<DataPage>(currentPageId);
            if (enoughSpaceForInsert<DataPage>(data.page(), rowSize))
            {
                auto LocatedRow = appendRowToExistingDataPage(header, data, row);
                LocatedRows.push_back(std::move(LocatedRow));
                break;
            }
            if (data.page().nextPageId() != 0)
                currentPageId = data.page().nextPageId();
            else
                currentPageId = createPage<DataPage>(header);
        }
    }
    return LocatedRows;
}

void BufferManager::flushPage(PageId pageId)
{
    requireReadable();
    PageFrame &frame = pages.at(pageId);

    if (!frame.dirty)
    {
        return;
    }

    try
    {
        statementRecovery_.validateCoverage(relativeTablePath_, pageId, frame.coverage);
        RawPage encodedPage = encodeCachedPage(frame.page);
        statementRecovery_.ensureDurable(frame.coverage);
        // A failing write may already have transferred a prefix.
        participant_->needsSync = true;
        pageFile_.writePage(pageId, encodedPage);
    }
    catch (...)
    {
        statementRecovery_.statementFailed();
        throw;
    }

    frame.dirty = false;
    participant_->dirtyPages.erase(pageId);
}

void BufferManager::flushAll()
{
    requireReadable();
    JournalCoverage required{};
    try
    {
        for (const auto &[pageId, frame] : pages)
        {
            if (frame.dirty)
            {
                statementRecovery_.validateCoverage(relativeTablePath_, pageId, frame.coverage);
                if (frame.coverage.end > required.end)
                    required = frame.coverage;
            }
        }
        if (required.generation == 0)
            return;
        statementRecovery_.ensureDurable(required);
        for (const auto &[pageId, frame] : pages)
        {
            if (frame.dirty)
            {
                const auto encoded = encodeCachedPage(frame.page);
                participant_->needsSync = true;
                pageFile_.writePage(pageId, encoded);
            }
        }
    }
    catch (...)
    {
        statementRecovery_.statementFailed();
        throw;
    }

    // All native writes completed before dirty flags are cleared. This is
    // writeback, not a durable statement commit; recovery coordination is separate.

    for (auto &[pageId, frame] : pages)
    {
        frame.dirty = false;
    }
    participant_->dirtyPages.clear();
}

void BufferManager::sync()
{
    requireReadable();
    try
    {
        pageFile_.sync();
        participant_->needsSync = false;
    }
    catch (...)
    {
        statementRecovery_.statementFailed();
        throw;
    }
}

void BufferManager::setPage(const Page &page, PageId pageId)
{
    requireReadable();
    const auto existing = pages.find(pageId);
    if (existing != pages.end() && existing->second.pinCount != 0)
    {
        throw std::runtime_error("Cannot replace a pinned page");
    }

    const auto coverage = statementRecovery_.requiredCoverageForPage(relativeTablePath_, pageId);
    statementRecovery_.trackParticipant(participant_);
    participant_->dirtyPages.insert(pageId);
    pages.insert_or_assign(
        pageId,
        PageFrame{
            .pageId = pageId,
            .page = page,
            .dirty = true,
            .coverage = coverage});
}

RawPage BufferManager::encodeCachedPage(const Page &page)
{
    if (page.header.pageType == PageType::HeaderPage || page.header.pageType == PageType::IndexHeaderPage)
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

    if (page.header.pageType == PageType::IndexPage)
        return encodeIndexPage(page, schema_);
    throw std::runtime_error("Unsupported page type while encoding cached page");
}

LocatedRow BufferManager::appendRowToExistingDataPage(
    PageGuard &header,
    PageGuard &data,
    const Row &row)
{
    HeaderPage &headerPage = header.write<HeaderPage>();
    DataPage &dataPage = data.write<DataPage>();
    PageHeader &dataPageHeader = data.writePage().header;

    const std::size_t encodedSize = encodedRowSize(headerPage, row);
    if (encodedSize > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::runtime_error("Row is too large for uint16_t slot size");
    }
    if (encodedSize > PAGE_SIZE)
    {
        throw std::runtime_error("Row is too large for page size");
    }

    const std::uint16_t rowSize = static_cast<std::uint16_t>(encodedSize);
    const std::size_t slotSize = encodedSlotSize();

    if (dataPage.slots.size() > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::runtime_error("Data page has too many slots");
    }
    dataPageHeader.slotCount =
        static_cast<std::uint16_t>(dataPage.slots.size());

    RowId rowId{};
    rowId.pageId = dataPageHeader.pageId;
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

            rowId.slotIndex = static_cast<std::uint16_t>(slotIndex);

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
        const auto slotIndex = static_cast<std::uint16_t>(dataPage.slots.size() - 1);
        dataPage.rows.push_back(RowEntry{
            .slotIndex = slotIndex,
            .row = row});

        rowId.slotIndex = slotIndex;

        dataPageHeader.freeSpaceStart = static_cast<std::uint16_t>(
            dataPageHeader.freeSpaceStart + rowSize);
        dataPageHeader.freeSpaceEnd = static_cast<std::uint16_t>(
            dataPageHeader.freeSpaceEnd - slotSize);
        dataPageHeader.slotCount =
            static_cast<std::uint16_t>(dataPage.slots.size());
    }

    ++headerPage.totalRowCount;
    return LocatedRow{.id = rowId, .row = row};
}

void BufferManager::requireNewIndex() const
{
    requireReadable();
    if (!indexFile_ || !statementRecovery_.isNewFile(relativeTablePath_))
        throw std::logic_error("Bulk loading requires a new index in the active statement");
}

void BufferManager::writeBulkIndexPage(const Page &page)
{
    requireNewIndex();
    if (page.header.pageType != PageType::IndexPage || page.pageId() == 0)
        throw std::logic_error("Expected a non-header index page");
    setPage(page, page.pageId());
    flushPage(page.pageId());
    pages.erase(page.pageId());
}

void BufferManager::initializeNewTable(const CreateTablePayload &createTablePayload)
{
    requireReadable();
    if (!statementRecovery_.isNewFile(relativeTablePath_) || pageFile_.size() != 0)
    {
        throw std::logic_error("Can only initialize a new, unwritten table in the active statement");
    }

    Page headerPage = makeHeaderPage(createTablePayload);
    schema_.columns = std::get<HeaderPage>(headerPage.data).columns;
    Page dataPage = makeEmptyPage<DataPage>(FirstDataPageId);
    setPage(headerPage, headerPage.pageId());
    setPage(dataPage, headerPage.nextPageId());
}

void BufferManager::initializeNewIndex(const CreateIndexPayload &createIndexPayload)
{
    requireReadable();
    if (!statementRecovery_.isNewFile(relativeTablePath_) || pageFile_.size() != 0)
    {
        throw std::logic_error("Can only initialize a new, unwritten table in the active statement");
    }

    Page headerPage = makeHeaderPage(createIndexPayload);
    schema_.columns = std::get<IndexHeaderPage>(headerPage.data).columns;
    Page dataPage = makeEmptyPage<IndexPage>(FirstDataPageId);
    setPage(headerPage, headerPage.pageId());
    setPage(dataPage, headerPage.nextPageId());
}

template <PagePayload T>
PageId BufferManager::createPage(PageGuard &header)
{
    HeaderPage &headerPage = header.write<HeaderPage>();

    PageId previousPageId = headerPage.lastDataPageId;
    PageId newPageId = headerPage.nextUnusedPageId;

    if (newPageId == 0)
    {
        if (previousPageId > std::numeric_limits<PageId>::max() - 1)
        {
            throw std::runtime_error("No more page IDs available");
        }

        newPageId = previousPageId + 1;
    }

    if (previousPageId != 0)
    {
        PageGuard previous = getPage<T>(previousPageId);
        previous.writePage().header.nextPageId = newPageId;
    }
    else
    {
        headerPage.firstDataPageId = newPageId;
    }

    Page newDataPage = makeEmptyPage<T>(newPageId);
    if (newDataPage.header.pageType != T::type())
    {
        throw std::runtime_error("New page has the wrong page type");
    }

    headerPage.lastDataPageId = newPageId;
    headerPage.nextUnusedPageId = newPageId + 1;
    setPage(newDataPage, newPageId);

    return newPageId;
}

template <PagePayload T>
bool BufferManager::enoughSpaceForInsert(
    const Page &page,
    std::size_t rowSize) const
{
    if (page.header.pageType != T::type())
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

    const T &dataPage = std::get<T>(page.data);
    for (const Slot &slot : dataPage.slots)
    {
        if (slot.has(SlotFlag::Deleted) && slot.size >= rowSize)
        {
            return true;
        }
    }

    return false;
}

// Index pages are read by Index, outside this translation unit.
template PageGuard BufferManager::getPage<IndexPage>(PageId);
