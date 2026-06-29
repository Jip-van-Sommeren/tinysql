#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <filesystem>
#include <fstream>
#include <format>
#include <cmath>
#include "db_storage.h"
#include "db_read.h"

template <typename T, typename Buffer>
T readUnsigned(const Buffer &buffer, size_t &offset)
{
    static_assert(std::is_unsigned_v<T>, "T must be an unsigned integer type");

    if (offset + sizeof(T) > buffer.size())
    {
        throw std::runtime_error("Not enough bytes to read unsigned integer");
    }

    T value = 0;

    for (size_t i = 0; i < sizeof(T); ++i)
    {
        uint8_t byte = std::to_integer<uint8_t>(buffer[offset + i]);
        value |= static_cast<T>(byte) << (i * 8);
    }

    offset += sizeof(T);

    return value;
}

template <typename T, typename Buffer>
T readSigned(const Buffer &buffer, size_t &offset)
{
    using UnsignedT = std::make_unsigned_t<T>;

    static_assert(std::is_signed_v<T>, "T must be a signed integer type");

    return static_cast<T>(readUnsigned<UnsignedT>(buffer, offset));
}

template <typename Buffer>
uint8_t readUInt8(const Buffer &buffer, size_t &offset)
{
    return readUnsigned<uint8_t>(buffer, offset);
}

template <typename Buffer>
uint16_t readUInt16(const Buffer &buffer, size_t &offset)
{
    return readUnsigned<uint16_t>(buffer, offset);
}
template <typename Buffer>
uint32_t readUInt32(const Buffer &buffer, size_t &offset)
{
    return readUnsigned<uint32_t>(buffer, offset);
}
template <typename Buffer>
uint64_t readUInt64(const Buffer &buffer, size_t &offset)
{
    return readUnsigned<uint64_t>(buffer, offset);
}

template <typename Buffer>
int32_t readInt32(const Buffer &buffer, size_t &offset)
{
    return readSigned<int32_t>(buffer, offset);
}

template <typename Buffer>
std::string readString(const Buffer &buffer, size_t &offset)
{
    uint32_t length = readUInt32(buffer, offset);

    if (offset + length > buffer.size())
    {
        throw std::runtime_error("Not enough bytes to read string");
    }

    std::string result;
    result.reserve(length);

    for (uint32_t i = 0; i < length; ++i)
    {
        char c = static_cast<char>(
            std::to_integer<uint8_t>(buffer[offset + i]));

        result.push_back(c);
    }

    offset += length;

    return result;
}

template <typename Buffer>
Value readValue(const Buffer &buffer, size_t &offset)
{
    uint8_t rawType = readUInt8(buffer, offset);
    DataType type = static_cast<DataType>(rawType);

    switch (type)
    {
    case DataType::Null:
        return Value{std::monostate{}};

    case DataType::Int:
        return Value{readInt32(buffer, offset)};

    case DataType::Text:
        return Value{readString(buffer, offset)};

    default:
        throw std::runtime_error("Invalid value data type");
    }
}

template <typename Buffer>
Row readRowAt(const Buffer &buffer, const Slot &slot)
{
    size_t offset = slot.offset;
    size_t rowEnd = slot.offset + slot.size;

    uint32_t valueCount = readUInt32(buffer, offset);

    std::vector<Value> values;
    values.reserve(valueCount);

    for (uint32_t i = 0; i < valueCount; ++i)
    {
        values.push_back(readValue(buffer, offset));
    }

    if (offset != rowEnd)
    {
        throw std::runtime_error("Row size mismatch while reading row");
    }

    return Row{
        .values = values};
}

template <typename Buffer>
Column readColumn(const Buffer &buffer, size_t &offset)
{
    std::string colName = readString(buffer, offset);

    DataType colType = static_cast<DataType>(readUInt8(buffer, offset));

    bool colNullable = static_cast<bool>(readUInt8(buffer, offset));

    return Column{
        .name = colName,
        .type = colType,
        .nullable = colNullable};
}

RawPage readPageFromFile(
    const std::filesystem::path &tablePath,
    uint32_t pageId)
{
    RawPage page{};

    std::ifstream file{tablePath, std::ios::binary};

    if (!file)
    {
        throw std::runtime_error("Failed to open file: " + tablePath.string());
    }

    file.seekg(static_cast<std::streamoff>(pageId) * PAGE_SIZE);

    file.read(
        reinterpret_cast<char *>(page.data()),
        static_cast<std::streamsize>(page.size()));

    if (!file)
    {
        throw std::runtime_error("Failed to read page " + std::to_string(pageId));
    }

    return page;
}

Page decodeHeaderPage(const RawPage &page)
{
    size_t offset = 0;

    PageHeader pageHeader = decodePageHeader(page, offset);

    if (pageHeader.pageType != PageType::TableHeader)
    {
        throw std::runtime_error("Page is not a header page");
    }

    std::string magic = readString(page, offset);
    uint16_t version = readUInt16(page, offset);
    uint32_t pageSize = readUInt32(page, offset);
    std::string tableName = readString(page, offset);

    uint32_t columnCount = readUInt32(page, offset);

    std::vector<Column> columns;
    columns.reserve(columnCount);

    for (uint32_t i = 0; i < columnCount; ++i)
    {
        columns.push_back(readColumn(page, offset));
    }

    uint64_t totalRowCount = readUInt64(page, offset);
    uint32_t firstDataPageId = readUInt32(page, offset);
    uint32_t lastDataPageId = readUInt32(page, offset);
    uint32_t nextPageId = readUInt32(page, offset);

    if (offset != pageHeader.freeSpaceStart)
    {
        throw std::runtime_error("Header page free space offset mismatch");
    }

    return Page{.header = pageHeader,
                .data = HeaderPage{
                    .magic = magic,
                    .version = version,
                    .pageSize = pageSize,
                    .tableName = tableName,
                    .columns = columns,
                    .totalRowCount = totalRowCount,
                    .firstDataPageId = firstDataPageId,
                    .lastDataPageId = lastDataPageId,
                    .nextUnusedPageId = nextPageId}};
}

Slot decodeSlot(const RawPage &page, size_t &offset)
{
    uint16_t slotOffset = readUInt16(page, offset);
    uint16_t size = readUInt16(page, offset);
    uint8_t deleted = readUInt8(page, offset);
    return Slot{.offset = slotOffset, .size = size, .deleted = deleted};
}

Page decodeDataPage(const RawPage &page)
{
    size_t offset = 0;

    PageHeader pageHeader = decodePageHeader(page, offset);

    if (pageHeader.pageType != PageType::Data)
    {
        throw std::runtime_error("Page is not a data page");
    }

    std::vector<RowEntry> rows;
    std::vector<Slot> slots;
    slots.reserve(pageHeader.slotCount);
    rows.reserve(pageHeader.slotCount);

    for (uint16_t i = 0; i < pageHeader.slotCount; ++i)
    {
        Slot slot = decodeSlot(page, offset);
        slots.push_back(slot);
        if (!slot.deleted)
        {
            rows.push_back(RowEntry{.slotIndex = i, .row = readRowAt(page, slot)});
        }
    }

    if (offset != pageHeader.freeSpaceStart)
    {
        throw std::runtime_error("Data page free space offset mismatch");
    }

    return Page{.header = pageHeader,
                .data = DataPage{
                    .slots = slots, .rows = rows}};
}

bool isValidPageType(uint8_t value)
{
    return value == static_cast<uint8_t>(PageType::TableHeader) ||
           value == static_cast<uint8_t>(PageType::Data) ||
           value == static_cast<uint8_t>(PageType::Index);
}

PageHeader decodePageHeader(const RawPage &page, size_t &offset)
{

    uint32_t pageId = readUInt32(page, offset);

    uint8_t rawPageType = readUInt8(page, offset);

    if (!isValidPageType(rawPageType))
    {
        throw std::runtime_error("Invalid page type: " + std::to_string(rawPageType));
    }

    PageType pageType = static_cast<PageType>(rawPageType);

    uint16_t slotCount = readUInt16(page, offset);
    uint16_t freeSpaceStart = readUInt16(page, offset);
    uint16_t freeSpaceEnd = readUInt16(page, offset);
    uint32_t nextPageId = readUInt32(page, offset);

    return PageHeader{
        .pageId = pageId,
        .pageType = pageType,
        .slotCount = slotCount,
        .freeSpaceStart = freeSpaceStart,
        .freeSpaceEnd = freeSpaceEnd,
        .nextPageId = nextPageId,
    };
}
