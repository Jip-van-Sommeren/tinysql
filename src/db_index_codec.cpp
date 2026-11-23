#include "db_index_codec.h"
#include "db_read.h"
#include "db_write.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace
{
    constexpr std::size_t KeyLengthBytes = sizeof(std::uint16_t);
    constexpr std::size_t RowIdBytes = sizeof(PageId) + sizeof(std::uint16_t);

    int compareValue(const Value &left, const Value &right)
    {
        if (std::holds_alternative<std::monostate>(left))
            return std::holds_alternative<std::monostate>(right) ? 0 : -1;
        if (std::holds_alternative<std::monostate>(right))
            return 1;
        if (left.index() != right.index())
            throw std::runtime_error("Index key value types do not match");
        return std::visit([&](const auto &a) -> int
                          {
            using T = std::decay_t<decltype(a)>;
            const auto &b = std::get<T>(right);
            if constexpr (std::is_floating_point_v<T>)
            {
                // A strict total ordering is required by sorting and tree routing.
                if (std::isnan(a)) return std::isnan(b) ? 0 : 1;
                if (std::isnan(b)) return -1;
            }
            return a < b ? -1 : b < a ? 1 : 0; }, left);
    }

    void writeKey(ByteWriter &writer, const Row &key, const RowSchema &schema)
    {
        const auto length = RowWriter::computeSerializedRowSize(schema, key.values);
        if (length > std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("Index key is too large");
        writer.writeUnsigned<std::uint16_t>(static_cast<std::uint16_t>(length));
        RowWriter(writer, schema).writeRow(key.values);
    }

    Row readKey(ByteReader &reader, std::span<const std::byte> bytes,
                const RowSchema &schema, std::size_t tailSize)
    {
        const auto length = reader.readUnsigned<std::uint16_t>();
        if (bytes.size() != KeyLengthBytes + length + tailSize)
            throw std::runtime_error("Invalid index entry size");
        ByteReader keyReader(bytes.subspan(KeyLengthBytes, length));
        Row key = RowDecoder(keyReader, schema, 0).decodeRow();
        if (RowWriter::computeSerializedRowSize(schema, key.values) != length)
            throw std::runtime_error("Invalid index key size");
        reader.seek(KeyLengthBytes + length);
        return key;
    }

    void writeRowId(ByteWriter &writer, RowId id)
    {
        writer.writeUnsigned<PageId>(id.pageId);
        writer.writeUnsigned<std::uint16_t>(id.slotIndex);
    }

    RowId readRowId(ByteReader &reader)
    {
        return {reader.readUnsigned<PageId>(), reader.readUnsigned<std::uint16_t>()};
    }
}

int compareIndexKeys(const Row &left, const Row &right)
{
    if (left.values.size() != right.values.size())
        throw std::runtime_error("Index key column counts do not match");
    for (std::size_t i = 0; i < left.values.size(); ++i)
        if (const int order = compareValue(left.values[i], right.values[i]); order != 0)
            return order;
    return 0;
}

int comparePartialIndexKeys(const Row &key, const Row &target)
{
    if (target.values.size() > key.values.size())
    {
        throw std::runtime_error("Target Key cannot be larger than the indexed key");
    }
    for (std::size_t i = 0; i < target.values.size(); ++i)
        if (const int order = compareValue(key.values[i], target.values[i]); order != 0)
            return order;
    return 0;
}

bool indexEntryLess(const LeafIndexEntry &left, const LeafIndexEntry &right)
{
    const int order = compareIndexKeys(left.key, right.key);
    return order < 0 || (order == 0 && left.rowId < right.rowId);
}

bool indexKeyHasNull(const Row &key)
{
    return std::ranges::any_of(key.values, [](const Value &value)
                               { return std::holds_alternative<std::monostate>(value); });
}

bool indexKeyHasNaN(const Row &key)
{
    return std::ranges::any_of(key.values, [](const Value &value)
                               { return std::visit([](const auto &v) -> bool
                                                   {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_floating_point_v<T>)
                return std::isnan(v);
            else
                return false; }, value); });
}

std::size_t LeafIndexEntry::serializedSize(const RowSchema &schema) const
{
    return KeyLengthBytes + RowWriter::computeSerializedRowSize(schema, key.values) + RowIdBytes;
}

std::size_t InternalIndexEntry::serializedSize(const RowSchema &schema) const
{
    return KeyLengthBytes + RowWriter::computeSerializedRowSize(schema, separatorKey.values) +
           RowIdBytes + sizeof(PageId);
}

IndexWriter::IndexWriter(ByteWriter &writer) : writer(writer) {}

void IndexWriter::writeIndex(const LeafIndexEntry &entry, const RowSchema &schema)
{
    writeKey(writer, entry.key, schema);
    writeRowId(writer, entry.rowId);
}

void IndexWriter::writeIndex(const InternalIndexEntry &entry, const RowSchema &schema)
{
    writeKey(writer, entry.separatorKey, schema);
    writeRowId(writer, entry.separatorRowId);
    writer.writeUnsigned<PageId>(entry.pageId);
}

LeafIndexEntry decodeLeafIndexEntry(std::span<const std::byte> bytes, const RowSchema &schema)
{
    ByteReader reader(bytes);
    Row key = readKey(reader, bytes, schema, RowIdBytes);
    return {std::move(key), readRowId(reader)};
}

InternalIndexEntry decodeInternalIndexEntry(std::span<const std::byte> bytes, const RowSchema &schema)
{
    ByteReader reader(bytes);
    Row key = readKey(reader, bytes, schema, RowIdBytes + sizeof(PageId));
    const RowId id = readRowId(reader);
    return {std::move(key), id, reader.readUnsigned<PageId>()};
}

IndexPageWriter::IndexPageWriter(ByteWriter &writer, const RowSchema &schema)
    : writer(writer), schema_(schema), headerWriter(writer), slotWriter(writer) {}

void IndexPageWriter::write(std::uint32_t pageId, const IndexEntry &entries)
{
    const bool leaf = std::holds_alternative<std::vector<LeafIndexEntry>>(entries);
    // Internal callers must supply their actual level and first child explicitly.
    if (!leaf)
        throw std::logic_error("Internal page requires a first child");
    PageHeader header{};
    header.pageId = pageId;
    header.pageType = PageType::IndexPage;
    write(header, IndexPageHeader{}, entries);
}

void IndexPageWriter::write(const PageHeader &pageHeader, const IndexPageHeader &indexHeader,
                            const IndexEntry &entries)
{
    const bool leaf = std::holds_alternative<std::vector<LeafIndexEntry>>(entries);
    if (leaf != (indexHeader.level == 0) || (leaf ? indexHeader.firstChildPageId != 0 : indexHeader.firstChildPageId == 0))
        throw std::runtime_error("Invalid index page children or level");
    PageHeader header = pageHeader;
    header.pageType = PageType::IndexPage;
    header.slotCount = 0;
    header.freeSpaceStart = static_cast<std::uint16_t>(PageHeaderLayout::Size + IndexPageHeader::serializedSize());
    header.freeSpaceEnd = static_cast<std::uint16_t>(PAGE_SIZE);
    headerWriter.write(header);
    writer.seek(PageHeaderLayout::Size);
    writer.writeUnsigned<std::uint16_t>(indexHeader.level);
    writer.writeUnsigned<std::uint16_t>(indexHeader.flags);
    writer.writeUnsigned<PageId>(indexHeader.firstChildPageId);
    std::visit([&](const auto &values)
               {
        for (const auto &entry : values)
        {
            const auto size = entry.serializedSize(schema_);
            if (size + SlotWriter::SlotSize > static_cast<std::size_t>(header.freeSpaceEnd - header.freeSpaceStart))
                throw std::runtime_error("Not enough space in index page");
            writer.seek(header.freeSpaceStart);
            IndexWriter(writer).writeIndex(entry, schema_);
            if (writer.position() != header.freeSpaceStart + size)
                throw std::logic_error("Computed index entry size does not match encoded size");
            slotWriter.writeSlot(header.slotCount, header.freeSpaceStart, static_cast<std::uint16_t>(size));
            ++header.slotCount;
            header.freeSpaceStart += static_cast<std::uint16_t>(size);
            header.freeSpaceEnd -= static_cast<std::uint16_t>(SlotWriter::SlotSize);
        } }, entries);
    headerWriter.write(header);
}

RawPage encodeIndexPage(const Page &page, const RowSchema &schema)
{
    const auto &index = std::get<IndexPage>(page.data);
    ByteWriter writer(PAGE_SIZE);
    IndexPageWriter(writer, schema).write(page.header, index.header, index.entries);
    RawPage raw{};
    std::copy(writer.bytes().begin(), writer.bytes().end(), raw.begin());
    return raw;
}

Page decodeIndexPage(const RawPage &raw, const RowSchema &schema)
{
    ByteReader reader(raw.bytes);
    const auto header = PageHeaderDecoder(reader).decode();
    constexpr auto start = PageHeaderLayout::Size + IndexPageHeader::serializedSize();
    if (header.pageType != PageType::IndexPage || header.slotCount > (PAGE_SIZE - start) / SlotWriter::SlotSize ||
        header.freeSpaceEnd != PAGE_SIZE - header.slotCount * SlotWriter::SlotSize ||
        header.freeSpaceStart < start || header.freeSpaceStart > header.freeSpaceEnd)
        throw std::runtime_error("Invalid index page header");
    IndexPage index{};
    index.header.level = reader.readUnsigned<std::uint16_t>();
    index.header.flags = reader.readUnsigned<std::uint16_t>();
    index.header.firstChildPageId = reader.readUnsigned<PageId>();
    const bool leaf = index.header.level == 0;
    if (leaf ? index.header.firstChildPageId != 0 : index.header.firstChildPageId == 0)
        throw std::runtime_error("Invalid index first child");
    if (!leaf)
        index.entries = std::vector<InternalIndexEntry>{};
    std::size_t offset = start;
    for (std::uint16_t i = 0; i < header.slotCount; ++i)
    {
        const Slot slot = SlotDecoder(reader).decodeSlot(i);
        if (slot.flags != 0 || slot.offset != offset || slot.size > header.freeSpaceStart - offset)
            throw std::runtime_error("Invalid index slot");
        const auto bytes = std::span<const std::byte>(raw.bytes).subspan(offset, slot.size);
        if (leaf)
            std::get<std::vector<LeafIndexEntry>>(index.entries).push_back(decodeLeafIndexEntry(bytes, schema));
        else
            std::get<std::vector<InternalIndexEntry>>(index.entries).push_back(decodeInternalIndexEntry(bytes, schema));
        index.slots.push_back(slot);
        offset += slot.size;
    }
    if (offset != header.freeSpaceStart)
        throw std::runtime_error("Invalid index payload boundary");
    return {header, std::move(index)};
}
