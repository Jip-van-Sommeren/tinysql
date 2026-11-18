#include "db_index.h"
#include "db_table.h"
#include "db_index_codec.h"
#include "db_row_validator.h"
#include "db_write.h"
#include <algorithm>
#include <utility>
#include <variant>

IndexCursor::IndexCursor(Index &index)
    : index_(&index)
{
    const PageGuard header = index_->bufferManager_.getIndexHeaderPage();
    pageId_ = header.as<IndexHeaderPage>().rootPageId;
    // Include allocated pages that have not been flushed yet.
    remainingPages_ = header.as<IndexHeaderPage>().nextUnusedPageId;
}

IndexCursor::IndexCursor(IndexCursor &&other) noexcept
    : index_(std::exchange(other.index_, nullptr)),
      pageId_(std::exchange(other.pageId_, 0)),
      rowIndex_(std::exchange(other.rowIndex_, 0)),
      currentPage_(std::move(other.currentPage_)),
      remainingPages_(std::exchange(other.remainingPages_, 0)),
      ancestorPages_(std::move(other.ancestorPages_))
{
    other.currentPage_.reset();
}

IndexCursor &IndexCursor::operator=(IndexCursor &&other) noexcept
{
    if (this != &other)
    {
        currentPage_ = std::move(other.currentPage_);
        other.currentPage_.reset();
        index_ = std::exchange(other.index_, nullptr);
        pageId_ = std::exchange(other.pageId_, 0);
        rowIndex_ = std::exchange(other.rowIndex_, 0);
        remainingPages_ = std::exchange(other.remainingPages_, 0);
        ancestorPages_ = std::move(other.ancestorPages_);
    }
    return *this;
}

void IndexCursor::seek(const Row &key, const RowId &targetRowId)
{
    if (!index_)
        return;
    index_->bufferManager_.requireReadable();

    currentPage_.reset();
    rowIndex_ = 0;
    ancestorPages_.clear();

    const auto &header =
        index_->bufferManager_.getIndexHeaderPage();

    const auto &metadata = header.as<IndexHeaderPage>();

    if (key.values.size() != metadata.columns.size())
    {
        throw std::invalid_argument(
            "Seeking by RowId requires a complete index key");
    }

    pageId_ = metadata.rootPageId;

    remainingPages_ = metadata.nextUnusedPageId;

    if (key.values.empty())
        return;

    while (pageId_ != 0)
    {
        if (!currentPage_)
        {
            if (remainingPages_ == 0)
            {
                throw std::runtime_error(
                    "Detected a cycle at page " + std::to_string(pageId_));
            }
            --remainingPages_;

            currentPage_.emplace(index_->bufferManager_.getPage<IndexPage>(pageId_));
        }

        const auto &page = currentPage_->as<IndexPage>();
        if (page.header.level > 0)
        {
            const auto &internalEntries =
                std::get<std::vector<InternalIndexEntry>>(page.entries);
            PageId child = page.header.firstChildPageId;
            const auto position = std::upper_bound(
                internalEntries.begin(),
                internalEntries.end(),
                key,
                [&targetRowId](const Row &target, const InternalIndexEntry &entry)
                {
                    const int order = compareIndexKeys(target, entry.separatorKey);

                    return order < 0 ||
                           (order == 0 && targetRowId < entry.separatorRowId);
                });

            const auto childIndex = static_cast<std::size_t>(
                std::distance(internalEntries.begin(), position));

            child = childIndex == 0
                        ? page.header.firstChildPageId
                        : internalEntries[childIndex - 1].pageId;
            ancestorPages_.push_back(IndexPathEntry{.parentPageId = pageId_, .childIndex = childIndex});
            pageId_ = child;
            currentPage_.reset();
            continue;
        }
        const auto &entries =
            std::get<std::vector<LeafIndexEntry>>(
                currentPage_->as<IndexPage>().entries);

        const auto position = std::lower_bound(
            entries.begin(),
            entries.end(),
            key,
            [targetRowId](const LeafIndexEntry &entry, const Row &target)
            {
                const int order = compareIndexKeys(entry.key, target);
                return order < 0 ||
                       (order == 0 && entry.rowId < targetRowId);
            });
        rowIndex_ = static_cast<std::size_t>(
            std::distance(entries.begin(), position));

        return;
    }

    return;
}

void IndexCursor::seek(const Row &key)
{
    if (!index_)
        return;
    index_->bufferManager_.requireReadable();

    currentPage_.reset();
    rowIndex_ = 0;
    ancestorPages_.clear();

    {
        const auto header =
            index_->bufferManager_.getIndexHeaderPage();

        pageId_ = header.as<IndexHeaderPage>().rootPageId;
        remainingPages_ = header.as<IndexHeaderPage>().nextUnusedPageId;
    }

    if (key.values.empty())
        return;

    while (pageId_ != 0)
    {
        if (!currentPage_)
        {
            if (remainingPages_ == 0)
            {
                throw std::runtime_error(
                    "Detected a cycle at page " + std::to_string(pageId_));
            }
            --remainingPages_;

            currentPage_.emplace(index_->bufferManager_.getPage<IndexPage>(pageId_));
        }

        const auto &page = currentPage_->as<IndexPage>();
        if (page.header.level > 0)
        {
            const auto &internalEntries =
                std::get<std::vector<InternalIndexEntry>>(page.entries);
            PageId child = page.header.firstChildPageId;
            const auto position = std::lower_bound(
                internalEntries.begin(),
                internalEntries.end(),
                key,
                [](const InternalIndexEntry &entry, const Row &target)
                {
                    return comparePartialIndexKeys(entry.separatorKey, target) < 0;
                });

            const auto childIndex = static_cast<std::size_t>(
                std::distance(internalEntries.begin(), position));

            child = childIndex == 0
                        ? page.header.firstChildPageId
                        : internalEntries[childIndex - 1].pageId;
            ancestorPages_.push_back(IndexPathEntry{.parentPageId = pageId_, .childIndex = childIndex});

            pageId_ = child;
            currentPage_.reset();
            continue;
        }
        const auto &entries =
            std::get<std::vector<LeafIndexEntry>>(
                currentPage_->as<IndexPage>().entries);

        const auto position = std::lower_bound(
            entries.begin(),
            entries.end(),
            key,
            [](const LeafIndexEntry &entry, const Row &target)
            {
                return comparePartialIndexKeys(entry.key, target) < 0;
            });
        rowIndex_ = static_cast<std::size_t>(
            std::distance(entries.begin(), position));

        return;
    }

    return;
}

void Index::insertIndex(const LeafIndexEntry &entry)
{
    const auto &schema = bufferManager_.getSchema();
    const auto validation = validateRowAgainstSchema(schema.columns, entry.key);
    if (!validation.valid)
        throw std::invalid_argument(validation.message);
    // Use the same key-size limit as bulk loading. Two separators must fit
    // in an internal node so a split can leave two children on each side.
    constexpr auto payloadBytes = PAGE_SIZE - PageHeaderLayout::Size - IndexPageHeader::serializedSize();
    if (entry.serializedSize(schema) + sizeof(PageId) + encodedSlotSize() > payloadBytes / 2)
        throw std::runtime_error("Index key is too large for an index page");

    const bool unique = bufferManager_.getIndexHeaderPage().as<IndexHeaderPage>().unique;
    if (unique && !indexKeyHasNull(entry.key))
    {
        auto existing = seekLowerBound(entry.key);
        const auto match = existing.next();
        if (match && compareIndexKeys(match->key, entry.key) == 0)
            throw std::runtime_error("UNIQUE INDEX can contain only unique non-NULL keys");
    }

    auto cursor = IndexCursor(*this);
    cursor.seek(entry.key, entry.rowId);
    if (!cursor.currentPage_)
        throw std::runtime_error("Index has no root leaf");
    const auto &entries = std::get<std::vector<LeafIndexEntry>>(
        cursor.currentPage_->as<IndexPage>().entries);
    if (cursor.rowIndex_ < entries.size() && entries[cursor.rowIndex_].rowId == entry.rowId &&
        compareIndexKeys(entries[cursor.rowIndex_].key, entry.key) == 0)
        throw std::runtime_error("Index entry already exists");

    PageId leftPageId = cursor.pageId_;
    std::uint16_t level = 0;
    auto split = bufferManager_.insertIntoLeaf(*cursor.currentPage_, cursor.rowIndex_, entry);
    cursor.currentPage_.reset();

    while (split)
    {
        if (cursor.ancestorPages_.empty())
        {
            bufferManager_.createNewRoot(leftPageId, level, *split);
            break;
        }
        const auto path = cursor.ancestorPages_.back();
        cursor.ancestorPages_.pop_back();
        auto parent = bufferManager_.getPage<IndexPage>(path.parentPageId);
        leftPageId = path.parentPageId;
        level = parent.as<IndexPage>().header.level;
        split = bufferManager_.insertIntoInternal(parent, path.childIndex, *split);
    }
}

std::optional<LeafIndexEntry> IndexCursor::next()
{
    if (!index_)
        return std::nullopt;
    index_->bufferManager_.requireReadable();
    while (pageId_ != 0)
    {
        if (!currentPage_)
        {
            if (remainingPages_ == 0)
            {
                throw std::runtime_error(
                    "Detected a cycle at page " + std::to_string(pageId_));
            }
            --remainingPages_;

            currentPage_.emplace(index_->bufferManager_.getPage<IndexPage>(pageId_));
        }

        const auto &page = currentPage_->as<IndexPage>();
        if (page.header.level > 0)
        {
            pageId_ = page.header.firstChildPageId;
            rowIndex_ = 0;
            currentPage_.reset();
            continue;
        }
        const auto &entries =
            std::get<std::vector<LeafIndexEntry>>(page.entries);

        if (rowIndex_ < entries.size())
        {
            // Copy the key and its original table RowId.
            return entries[rowIndex_++];
        }

        pageId_ = currentPage_->page().nextPageId();
        rowIndex_ = 0;
        currentPage_.reset();
    }

    return std::nullopt;
}

Index::Index(const LinuxDirectory &indexDir,
             const std::filesystem::path &relIndexPath,
             LinuxFile::OpenMode mode,
             StatementRecovery &statementRecovery,
             std::optional<std::vector<Column>> schema)
    : bufferManager_(indexDir, relIndexPath, mode, statementRecovery, std::move(schema), true),
      statementRecovery_(statementRecovery)
{
}

Index Index::open(const LinuxDirectory &indexDir,
                  const std::filesystem::path &relPath,
                  StatementRecovery &statementRecovery)
{
    Index index{indexDir, relPath, LinuxFile::OpenMode::OpenExisting, statementRecovery, std::nullopt};
    index.validateHeaderPage();
    return index;
}

Index Index::create(const LinuxDirectory &indexDir,
                    const CreateIndexPayload &createIndexPayload,
                    StatementRecovery &statementRecovery)
{
    Index index{indexDir,
                createIndexPayload.indexName + ".index",
                LinuxFile::OpenMode::CreateNew,
                statementRecovery,
                createIndexPayload.columns};
    index.initializeNewIndex(createIndexPayload);
    return index;
}

void Index::validateHeaderPage()
{
    const auto &header = bufferManager_.getIndexHeaderPage();
    const auto &metadata = header.as<IndexHeaderPage>();
    tableName_ = metadata.tableName;
    indexName_ = metadata.indexName;
}

void Index::initializeNewIndex(const CreateIndexPayload &createIndexPayload)
{
    bufferManager_.initializeNewIndex(createIndexPayload);
    bufferManager_.flushAll();
    bufferManager_.sync();
}

void Index::deleteIndexes(const std::vector<LocatedRow> &rows)
{
    bufferManager_.requireReadable();
    if (rows.empty())
        return;
    try
    {
        auto cursor = IndexCursor(*this);
        const auto columnIds = bufferManager_.getIndexHeaderPage().as<IndexHeaderPage>().keyColumns;
        for (const auto &locRow : rows)
        {
            Row key{};
            key.values.reserve(columnIds.size());
            for (ColumnId id : columnIds)
                key.values.push_back(locRow.row.values.at(id));
            cursor.seek(key, locRow.id);
            if (!cursor.currentPage_)
                continue;
            const auto &entries = std::get<std::vector<LeafIndexEntry>>(
                cursor.currentPage_->as<IndexPage>().entries);
            // A lower bound can be the successor or the end of this leaf.
            // Delete only the exact (key, table RowId) requested by the caller.
            if (cursor.rowIndex_ == entries.size())
                continue;
            const auto &entry = entries[cursor.rowIndex_];
            if (entry.rowId == locRow.id && compareIndexKeys(entry.key, key) == 0)
                bufferManager_.deleteIndex(cursor.pageId_, cursor.rowIndex_);
        }
        bufferManager_.flushAll();
        bufferManager_.sync();
    }
    catch (...)
    {
        statementRecovery_.statementFailed();
        throw;
    }
}

void Index::insertIndexes(std::vector<LocatedRow> rows)
{
    bufferManager_.requireReadable();
    try
    {
        const auto columnIds = bufferManager_.getIndexHeaderPage().as<IndexHeaderPage>().keyColumns;
        for (const auto &locRow : rows)
        {
            Row key{};
            key.values.reserve(columnIds.size());
            for (ColumnId id : columnIds)
                key.values.push_back(locRow.row.values.at(id));
            insertIndex(LeafIndexEntry{std::move(key), locRow.id});
        }
        bufferManager_.flushAll();
        bufferManager_.sync();
    }
    catch (...)
    {
        statementRecovery_.statementFailed();
        throw;
    }
}

void Index::insertIndexes(std::vector<LeafIndexEntry> indexValues)
{
    bufferManager_.requireReadable();
    try
    {
        for (const auto &entry : indexValues)
            insertIndex(entry);
        bufferManager_.flushAll();
        bufferManager_.sync();
    }
    catch (...)
    {
        statementRecovery_.statementFailed();
        throw;
    }
}

IndexBulkLoadStats Index::bulkLoad(Table &table, const IndexBulkLoadOptions &options)
{
    const auto columnIds = bufferManager_.getIndexHeaderPage().as<IndexHeaderPage>().keyColumns;
    auto cursor = table.scan();
    return bulkLoad([&]() -> std::optional<LeafIndexEntry>
                    {
        auto row = cursor.nextLocated();
        if (!row) return std::nullopt;
        Row key;
        key.values.reserve(columnIds.size());
        for (ColumnId id : columnIds)
            key.values.push_back(row->row.values.at(id));
        return LeafIndexEntry{std::move(key), row->id}; }, options);
}

IndexCursor Index::scan()
{
    return IndexCursor(*this);
}

IndexCursor Index::seekLowerBound(const Row &key) &
{
    IndexCursor cursor(*this);
    cursor.seek(key);
    return cursor;
}
