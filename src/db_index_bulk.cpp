#include "db_index.h"
#include "db_index_codec.h"
#include "db_page_factory.h"
#include "db_read.h"
#include "db_write.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unistd.h>

namespace
{
    constexpr std::size_t NodeBytes = PAGE_SIZE - PageHeaderLayout::Size - IndexPageHeader::serializedSize();
    constexpr std::size_t RunHeaderBytes = 2 * sizeof(std::uint64_t);

    // Anonymous files are removed even if the process crashes. There are only
    // two spool files per sort pass, regardless of the number of runs.
    class TemporaryFile
    {
    public:
        TemporaryFile() : file_(std::tmpfile())
        {
            if (!file_)
                throw std::system_error(errno, std::generic_category(), "Create index sort temporary file");
        }
        std::uint64_t size() const { return size_; }
        void write(std::uint64_t offset, std::span<const std::byte> bytes)
        {
            checkRange(offset, bytes.size());
            std::size_t done = 0;
            while (done < bytes.size())
            {
                const auto written = ::pwrite(::fileno(file_.get()), bytes.data() + done, bytes.size() - done,
                                              static_cast<off_t>(offset + done));
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    throw std::system_error(written < 0 ? errno : EIO, std::generic_category(), "Write index sort temporary file");
                done += static_cast<std::size_t>(written);
            }
            size_ = std::max(size_, offset + bytes.size());
        }
        void read(std::uint64_t offset, std::span<std::byte> bytes) const
        {
            checkRange(offset, bytes.size());
            if (offset > size_ || bytes.size() > size_ - offset)
                throw std::runtime_error("Truncated index sort temporary file");
            std::size_t done = 0;
            while (done < bytes.size())
            {
                const auto count = ::pread(::fileno(file_.get()), bytes.data() + done, bytes.size() - done,
                                           static_cast<off_t>(offset + done));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count < 0)
                    throw std::system_error(errno, std::generic_category(), "Read index sort temporary file");
                if (count == 0)
                    throw std::runtime_error("Truncated index sort temporary file");
                done += static_cast<std::size_t>(count);
            }
        }

    private:
        struct CloseFile
        {
            void operator()(std::FILE *file) const noexcept { std::fclose(file); }
        };
        std::unique_ptr<std::FILE, CloseFile> file_;
        std::uint64_t size_ = 0;
        static void checkRange(std::uint64_t offset, std::size_t size)
        {
            constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
            if (offset > limit || size > limit - offset)
                throw std::overflow_error("Index sort file is too large");
        }
    };

    template <typename Entry>
    void appendEntry(TemporaryFile &file, const Entry &entry, const RowSchema &schema)
    {
        ByteWriter writer(entry.serializedSize(schema));
        IndexWriter(writer).writeIndex(entry, schema);
        file.write(file.size(), writer.bytes());
    }

    template <typename Entry>
    Entry readEntry(const TemporaryFile &file, std::uint64_t &offset, std::uint64_t end, const RowSchema &schema)
    {
        if (offset > end || end - offset < sizeof(std::uint16_t))
            throw std::runtime_error("Truncated index sort record");
        std::array<std::byte, sizeof(std::uint16_t)> prefix{};
        file.read(offset, prefix);
        const auto keySize = ByteReader(prefix).readUnsigned<std::uint16_t>();
        constexpr std::size_t overhead = sizeof(std::uint16_t) + sizeof(PageId) + sizeof(std::uint16_t) +
                                         (std::same_as<Entry, InternalIndexEntry> ? sizeof(PageId) : 0);
        const auto size = keySize + overhead;
        if (size > PAGE_SIZE || size > end - offset)
            throw std::runtime_error("Invalid index sort record size");
        std::vector<std::byte> bytes(size);
        file.read(offset, bytes);
        offset += size;
        if constexpr (std::same_as<Entry, LeafIndexEntry>)
            return decodeLeafIndexEntry(bytes, schema);
        else
            return decodeInternalIndexEntry(bytes, schema);
    }

    struct Run
    {
        std::uint64_t position;
        std::uint64_t end;
        std::uint64_t remaining;
    };

    std::uint64_t startRun(TemporaryFile &file)
    {
        const auto offset = file.size();
        std::array<std::byte, RunHeaderBytes> empty{};
        file.write(offset, empty);
        return offset;
    }

    void finishRun(TemporaryFile &file, std::uint64_t start, std::uint64_t count)
    {
        ByteWriter writer(RunHeaderBytes);
        writer.writeUnsigned<std::uint64_t>(file.size() - start - RunHeaderBytes);
        writer.writeUnsigned<std::uint64_t>(count);
        file.write(start, writer.bytes());
    }

    Run readRun(const TemporaryFile &file, std::uint64_t &offset)
    {
        std::array<std::byte, RunHeaderBytes> bytes{};
        file.read(offset, bytes);
        ByteReader reader(bytes);
        const auto length = reader.readUnsigned<std::uint64_t>();
        const auto count = reader.readUnsigned<std::uint64_t>();
        offset += RunHeaderBytes;
        if (length > file.size() - offset || count == 0)
            throw std::runtime_error("Invalid index sort run");
        Run run{offset, offset + length, count};
        offset += length;
        return run;
    }

    std::optional<LeafIndexEntry> nextEntry(const TemporaryFile &file, Run &run, const RowSchema &schema)
    {
        if (run.remaining == 0)
        {
            if (run.position != run.end)
                throw std::runtime_error("Invalid index sort run boundary");
            return std::nullopt;
        }
        auto entry = readEntry<LeafIndexEntry>(file, run.position, run.end, schema);
        --run.remaining;
        return entry;
    }

    // Includes an allowance for vector growth and string terminators/capacity.
    std::size_t memoryCharge(const LeafIndexEntry &entry)
    {
        std::size_t size = 2 * sizeof(LeafIndexEntry) + entry.key.values.capacity() * sizeof(Value);
        for (const auto &value : entry.key.values)
            if (const auto *text = std::get_if<std::string>(&value))
                size += text->capacity() + 1;
        return size;
    }

    struct SortedEntries
    {
        TemporaryFile file;
        Run run{};
    };

    SortedEntries sortEntries(Index::EntrySource &next, const RowSchema &schema,
                              const IndexBulkLoadOptions &options, IndexBulkLoadStats &stats)
    {
        // A decoded head and its transient encoded record must fit for each input.
        const auto headBudget = 2 * PAGE_SIZE + schema.columns.size() * sizeof(Value) + 2 * sizeof(LeafIndexEntry);
        if (options.mergeFanIn < 2 || options.mergeFanIn > 64 || options.memoryBudgetBytes / headBudget < 2)
            throw std::invalid_argument("Index sort needs memory for at least two merge heads and a fan-in between 2 and 64");
        const auto fanIn = std::min(options.mergeFanIn, options.memoryBudgetBytes / headBudget);
        TemporaryFile input;
        std::vector<LeafIndexEntry> batch;
        std::size_t batchBytes = 0;
        auto spill = [&]
        {
            if (batch.empty())
                return;
            std::sort(batch.begin(), batch.end(), indexEntryLess);
            const auto start = startRun(input);
            for (const auto &entry : batch)
                appendEntry(input, entry, schema);
            finishRun(input, start, batch.size());
            ++stats.initialRuns;
            // Release capacity too: later batches may contain much larger keys.
            std::vector<LeafIndexEntry>{}.swap(batch);
            batchBytes = 0;
        };
        while (auto entry = next())
        {
            // Guarantee room for two separators, so internal pages have fanout
            // of at least three and a trailing singleton can be redistributed.
            if (entry->serializedSize(schema) + sizeof(PageId) + encodedSlotSize() > NodeBytes / 2)
                throw std::runtime_error("Index key is too large for an index page");
            const auto charge = memoryCharge(*entry);
            if (charge > options.memoryBudgetBytes)
                throw std::runtime_error("Index sort memory budget is too small for one entry");
            if (charge > options.memoryBudgetBytes - batchBytes)
                spill();
            batchBytes += charge;
            batch.push_back(std::move(*entry));
            ++stats.rows;
        }
        spill();
        auto runCount = stats.initialRuns;
        while (runCount > 1)
        {
            TemporaryFile output;
            std::uint64_t offset = 0;
            std::uint64_t outputRuns = 0;
            while (offset < input.size())
            {
                std::vector<Run> runs;
                std::vector<std::optional<LeafIndexEntry>> heads;
                runs.reserve(fanIn);
                heads.reserve(fanIn);
                while (runs.size() < fanIn && offset < input.size())
                {
                    runs.push_back(readRun(input, offset));
                    heads.push_back(nextEntry(input, runs.back(), schema));
                }
                auto later = [&](std::size_t a, std::size_t b)
                {
                    return indexEntryLess(*heads[b], *heads[a]);
                };
                std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(later)> queue(later);
                for (std::size_t i = 0; i < heads.size(); ++i)
                    queue.push(i);
                const auto start = startRun(output);
                std::uint64_t count = 0;
                while (!queue.empty())
                {
                    const auto i = queue.top();
                    queue.pop();
                    appendEntry(output, *heads[i], schema);
                    ++count;
                    heads[i] = nextEntry(input, runs[i], schema);
                    if (heads[i])
                        queue.push(i);
                }
                finishRun(output, start, count);
                ++outputRuns;
            }
            input = std::move(output);
            runCount = outputRuns;
            ++stats.mergePasses;
        }
        Run run{};
        if (runCount != 0)
        {
            std::uint64_t offset = 0;
            run = readRun(input, offset);
        }
        return {std::move(input), run};
    }

    struct ChildLevel
    {
        TemporaryFile file;
        std::uint64_t count = 0;
    };
}

IndexBulkLoadStats Index::bulkLoad(EntrySource next, const IndexBulkLoadOptions &options)
{
    bufferManager_.requireNewIndex();
    const auto metadata = bufferManager_.getIndexHeaderPage().as<IndexHeaderPage>();
    {
        const auto root = bufferManager_.getPage<IndexPage>(metadata.rootPageId);
        if (metadata.nextUnusedPageId != 2 || root.as<IndexPage>().header.level != 0 ||
            !std::get<std::vector<LeafIndexEntry>>(root.as<IndexPage>().entries).empty())
            throw std::logic_error("Bulk loading requires an empty index");
    }
    try
    {
        const RowSchema schema{metadata.columns};
        IndexBulkLoadStats stats;
        auto sorted = sortEntries(next, schema, options, stats);
        PageId nextPageId = 1;
        auto allocate = [&]()
        {
            if (nextPageId == std::numeric_limits<PageId>::max())
                throw std::overflow_error("No more index page IDs available");
            return nextPageId++;
        };
        ChildLevel children;
        Page leaf = makeEmptyPage<IndexPage>(allocate());
        std::size_t used = 0;
        auto finishLeaf = [&]
        {
            const auto &entries = std::get<std::vector<LeafIndexEntry>>(std::get<IndexPage>(leaf.data).entries);
            if (!entries.empty())
            {
                const auto &first = entries.front();
                appendEntry(children.file, InternalIndexEntry{first.key, first.rowId, leaf.pageId()}, schema);
                ++children.count;
            }
            bufferManager_.writeBulkIndexPage(leaf);
            ++stats.leafPages;
        };
        std::optional<Row> previousKey;
        while (auto entry = nextEntry(sorted.file, sorted.run, schema))
        {
            if (metadata.unique && !indexKeyHasNull(entry->key) && previousKey &&
                compareIndexKeys(*previousKey, entry->key) == 0)
                throw std::runtime_error("UNIQUE INDEX can contain only unique non-NULL keys");
            if (metadata.unique)
                previousKey = entry->key;
            const auto size = entry->serializedSize(schema) + encodedSlotSize();
            if (used + size > NodeBytes)
            {
                const auto id = allocate();
                leaf.header.nextPageId = id;
                finishLeaf();
                leaf = makeEmptyPage<IndexPage>(id);
                used = 0;
            }
            std::get<std::vector<LeafIndexEntry>>(std::get<IndexPage>(leaf.data).entries).push_back(std::move(*entry));
            used += size;
        }
        finishLeaf(); // An empty table still has one empty root leaf.
        PageId rootId = leaf.pageId();
        std::uint16_t level = 0;
        while (children.count > 1)
        {
            if (level == std::numeric_limits<std::uint16_t>::max())
                throw std::overflow_error("Index tree is too tall");
            ++level;
            ChildLevel parents;
            std::uint64_t offset = 0;
            auto first = readEntry<InternalIndexEntry>(children.file, offset, children.file.size(), schema);
            auto newParent = [&](const InternalIndexEntry &minimum)
            {
                Page page = makeEmptyPage<IndexPage>(allocate());
                auto &index = std::get<IndexPage>(page.data);
                index.header.level = level;
                index.header.firstChildPageId = minimum.pageId;
                index.entries = std::vector<InternalIndexEntry>{};
                return page;
            };
            Page parent = newParent(first);
            used = 0;
            auto finishParent = [&]
            {
                appendEntry(parents.file, InternalIndexEntry{first.separatorKey, first.separatorRowId, parent.pageId()}, schema);
                ++parents.count;
                bufferManager_.writeBulkIndexPage(parent);
                rootId = parent.pageId();
                ++stats.internalPages;
            };
            for (std::uint64_t i = 1; i < children.count; ++i)
            {
                auto child = readEntry<InternalIndexEntry>(children.file, offset, children.file.size(), schema);
                const auto size = child.serializedSize(schema) + encodedSlotSize();
                if (used + size > NodeBytes)
                {
                    auto &entries = std::get<std::vector<InternalIndexEntry>>(std::get<IndexPage>(parent.data).entries);
                    if (i + 1 == children.count)
                    {
                        // Avoid a final internal page with only one child.
                        auto displaced = std::move(entries.back());
                        entries.pop_back();
                        finishParent();
                        first = std::move(displaced);
                        parent = newParent(first);
                        used = 0;
                    }
                    else
                    {
                        finishParent();
                        first = std::move(child);
                        parent = newParent(first);
                        used = 0;
                        continue;
                    }
                }
                std::get<std::vector<InternalIndexEntry>>(std::get<IndexPage>(parent.data).entries).push_back(std::move(child));
                used += size;
            }
            finishParent();
            children = std::move(parents);
        }
        {
            auto header = bufferManager_.getIndexHeaderPage();
            auto &stored = header.write<IndexHeaderPage>();
            stored.rootPageId = rootId;
            stored.nextUnusedPageId = nextPageId;
        }
        bufferManager_.flushAll();
        bufferManager_.sync();
        return stats;
    }
    catch (...)
    {
        statementRecovery_.statementFailed();
        throw;
    }
}
