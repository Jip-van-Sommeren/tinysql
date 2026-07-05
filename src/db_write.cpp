#include "db_write.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <variant>

namespace
{
constexpr std::size_t VarEntrySize = 8;

template <typename T>
void appendUnsigned(std::vector<std::byte> &buffer, T value)
{
    static_assert(std::is_unsigned_v<T>, "T must be an unsigned integer type");

    for (std::size_t shift = 0; shift < sizeof(T) * 8; shift += 8)
    {
        buffer.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

void appendString(std::vector<std::byte> &buffer, const std::string &value)
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error("String too large to write");
    }

    appendUnsigned<std::uint32_t>(buffer, static_cast<std::uint32_t>(value.size()));

    const auto *data = reinterpret_cast<const std::byte *>(value.data());
    buffer.insert(buffer.end(), data, data + value.size());
}

std::size_t computeSerializedRowSize(const HeaderPage &tableHeader, const std::vector<Value> &values)
{
    if (values.size() != tableHeader.columns.size())
    {
        throw std::runtime_error("value count does not match column count");
    }

    std::size_t nullBitmapSize = (tableHeader.columns.size() + 7) / 8;
    std::size_t fixedAreaSize = 0;
    std::size_t varColumnCount = 0;
    std::size_t varDataSize = 0;

    for (const Column &column : tableHeader.columns)
    {
        const Value &value = values[column.columnIndex];

        if (std::holds_alternative<FixedColumnStorage>(column.storage))
        {
            const FixedColumnStorage &fixed = std::get<FixedColumnStorage>(column.storage);
            fixedAreaSize = std::max<std::size_t>(fixedAreaSize, fixed.offset + fixed.size);
            continue;
        }

        if (std::holds_alternative<VarColumnStorage>(column.storage))
        {
            ++varColumnCount;

            if (std::holds_alternative<std::monostate>(value))
            {
                continue;
            }

            if (column.type != DataType::Text || !std::holds_alternative<std::string>(value))
            {
                throw std::runtime_error("unsupported variable-length column value: " + column.name);
            }

            varDataSize += std::get<std::string>(value).size();
            continue;
        }

        throw std::runtime_error("unknown column storage type");
    }

    return nullBitmapSize + fixedAreaSize + (varColumnCount * VarEntrySize) + varDataSize;
}
}

PageWriter::PageWriter(RawPage &buffer)
    : buffer(buffer) {}

std::size_t PageWriter::position() const
{
    return pos;
}

void PageWriter::seek(std::size_t newPos)
{
    if (newPos > PAGE_SIZE)
    {
        throw std::runtime_error("seek past page boundary");
    }

    pos = newPos;
}

void PageWriter::writeBytes(const void *data, std::size_t size)
{
    ensureCapacity(size);

    const auto *bytes = static_cast<const std::byte *>(data);
    std::copy(bytes, bytes + size, buffer.begin() + pos);
    pos += size;
}

void PageWriter::writeBytesAt(std::size_t offset, const void *data, std::size_t size)
{
    std::size_t saved = pos;
    seek(offset);
    writeBytes(data, size);
    seek(saved);
}

void PageWriter::writeString(const std::string &value)
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error("String too large to write");
    }

    writeUnsigned<std::uint32_t>(static_cast<std::uint32_t>(value.size()));
    writeBytes(value.data(), value.size());
}

void PageWriter::ensureCapacity(std::size_t size) const
{
    if (pos + size > PAGE_SIZE)
    {
        throw std::runtime_error("write past page boundary");
    }
}

void ValueSerializer::writeFixed(
    PageWriter &writer,
    std::size_t absoluteOffset,
    DataType type,
    const Value &value)
{
    switch (type)
    {
    case DataType::Int:
        if (!std::holds_alternative<int>(value))
        {
            throw std::runtime_error("Expected int value");
        }

        writer.writeUnsignedAt<std::uint32_t>(
            absoluteOffset,
            static_cast<std::uint32_t>(std::get<int>(value)));
        return;

    default:
        throw std::runtime_error("Unsupported fixed-width type");
    }
}

std::vector<std::byte> ValueSerializer::serializeVariable(
    DataType type,
    const Value &value)
{
    switch (type)
    {
    case DataType::Text:
    {
        if (!std::holds_alternative<std::string>(value))
        {
            throw std::runtime_error("Expected string value");
        }

        const std::string &s = std::get<std::string>(value);
        const auto *data = reinterpret_cast<const std::byte *>(s.data());
        return std::vector<std::byte>(data, data + s.size());
    }

    default:
        throw std::runtime_error("Unsupported variable-width type");
    }
}

RowWriter::RowWriter(PageWriter &writer, const HeaderPage &headerPage)
    : writer(writer), headerPage(headerPage) {}

void RowWriter::writeRow(const std::vector<Value> &values)
{
    if (values.size() != headerPage.columns.size())
    {
        throw std::runtime_error("value count does not match column count");
    }

    prepareLayout();

    rowStart = writer.position();
    varDataPos = rowStart + varDataStartOffset;

    std::vector<std::uint8_t> nullBitmap(nullBitmapSizeBytes, 0);

    for (const Column &column : headerPage.columns)
    {
        const Value &value = values[column.columnIndex];

        if (std::holds_alternative<std::monostate>(value))
        {
            if (!column.nullable)
            {
                throw std::runtime_error("NULL provided for NOT NULL column: " + column.name);
            }

            setBitmapBit(nullBitmap, column.columnIndex);
        }
    }

    writer.writeBytes(nullBitmap.data(), nullBitmap.size());

    std::vector<std::byte> zeros(fixedAreaSize + varDirSize, std::byte{0});
    writer.writeBytes(zeros.data(), zeros.size());

    writeFixedValues(values);

    writer.seek(varDataPos);
    writeVariableValues(values);

    writer.writeBytesAt(rowStart, nullBitmap.data(), nullBitmap.size());
    writer.seek(varDataPos);
}

void RowWriter::setBitmapBit(std::vector<std::uint8_t> &bitmap, std::size_t bitIndex)
{
    bitmap[bitIndex / 8] |= static_cast<std::uint8_t>(1u << (bitIndex % 8));
}

void RowWriter::prepareLayout()
{
    nullBitmapSizeBytes = (headerPage.columns.size() + 7) / 8;

    fixedAreaSize = 0;
    std::size_t varCount = 0;

    for (const Column &column : headerPage.columns)
    {
        if (std::holds_alternative<FixedColumnStorage>(column.storage))
        {
            const FixedColumnStorage &fixed = std::get<FixedColumnStorage>(column.storage);
            fixedAreaSize = std::max<std::size_t>(fixedAreaSize, fixed.offset + fixed.size);
        }
        else
        {
            ++varCount;
        }
    }

    fixedAreaStartOffset = nullBitmapSizeBytes;
    varDirStartOffset = fixedAreaStartOffset + fixedAreaSize;
    varDirSize = varCount * VarEntrySize;
    varDataStartOffset = varDirStartOffset + varDirSize;
}

void RowWriter::writeFixedValues(const std::vector<Value> &values)
{
    for (const Column &column : headerPage.columns)
    {
        if (!std::holds_alternative<FixedColumnStorage>(column.storage))
        {
            continue;
        }

        const Value &value = values[column.columnIndex];

        if (std::holds_alternative<std::monostate>(value))
        {
            continue;
        }

        const FixedColumnStorage &fixed = std::get<FixedColumnStorage>(column.storage);
        writeFixedValue(column, fixed, value);
    }
}

void RowWriter::writeFixedValue(
    const Column &column,
    const FixedColumnStorage &fixed,
    const Value &value)
{
    std::size_t absoluteOffset = rowStart + fixedAreaStartOffset + fixed.offset;
    ValueSerializer::writeFixed(writer, absoluteOffset, column.type, value);
}

void RowWriter::writeVariableValues(const std::vector<Value> &values)
{
    for (const Column &column : headerPage.columns)
    {
        if (!std::holds_alternative<VarColumnStorage>(column.storage))
        {
            continue;
        }

        const Value &value = values[column.columnIndex];

        if (std::holds_alternative<std::monostate>(value))
        {
            continue;
        }

        const VarColumnStorage &var = std::get<VarColumnStorage>(column.storage);
        writeVariableValue(column, var, value);
    }
}

void RowWriter::writeVariableValue(
    const Column &column,
    const VarColumnStorage &var,
    const Value &value)
{
    std::vector<std::byte> bytes = ValueSerializer::serializeVariable(column.type, value);

    std::uint32_t relativeOffset = static_cast<std::uint32_t>(varDataPos - rowStart);
    std::uint32_t length = static_cast<std::uint32_t>(bytes.size());

    std::size_t varEntryOffset = rowStart + varDirStartOffset + var.varIndex * VarEntrySize;

    writer.writeUnsignedAt<std::uint32_t>(varEntryOffset, relativeOffset);
    writer.writeUnsignedAt<std::uint32_t>(varEntryOffset + 4, length);

    writer.seek(varDataPos);
    writer.writeBytes(bytes.data(), bytes.size());

    varDataPos = writer.position();
}

BitmapWriter::BitmapWriter(std::span<std::uint8_t> bytes)
    : bytes(bytes) {}

void BitmapWriter::set(std::size_t bitIndex)
{
    bytes[bitIndex / 8] |= static_cast<std::uint8_t>(1u << (bitIndex % 8));
}

void BitmapWriter::clear(std::size_t bitIndex)
{
    bytes[bitIndex / 8] &= static_cast<std::uint8_t>(~(1u << (bitIndex % 8)));
}

bool BitmapWriter::get(std::size_t bitIndex) const
{
    return (bytes[bitIndex / 8] &
            static_cast<std::uint8_t>(1u << (bitIndex % 8))) != 0;
}

SlotWriter::SlotWriter(PageWriter &writer)
    : writer(writer) {}

void SlotWriter::writeSlot(std::uint16_t slotIndex, const Slot &slot)
{
    std::size_t base = slotOffset(slotIndex);

    writer.writeUnsignedAt<std::uint16_t>(base, slot.offset);
    writer.writeUnsignedAt<std::uint16_t>(base + 2, slot.size);
    writer.writeUnsignedAt<std::uint16_t>(base + 4, slot.flags);
}

void SlotWriter::writeSlot(
    std::uint16_t slotIndex,
    std::uint16_t rowOffset,
    std::uint16_t rowSize,
    std::uint16_t flags)
{
    Slot slot{
        .offset = rowOffset,
        .size = rowSize,
        .flags = flags};

    writeSlot(slotIndex, slot);
}

void SlotWriter::markDeleted(std::uint16_t slotIndex, Slot slot)
{
    slot.set(SlotFlag::Deleted);
    writeSlot(slotIndex, slot);
}

std::size_t SlotWriter::slotOffset(std::uint16_t slotIndex) const
{
    return PAGE_SIZE - ((static_cast<std::size_t>(slotIndex) + 1) * SlotSize);
}

PageHeaderWriter::PageHeaderWriter(PageWriter &writer)
    : writer(writer) {}

void PageHeaderWriter::write(const PageHeader &header)
{
    writer.writeUnsignedAt<std::uint32_t>(PageHeaderLayout::PageId, header.pageId);
    writer.writeUnsignedAt<std::uint8_t>(
        PageHeaderLayout::PageType,
        static_cast<std::uint8_t>(header.pageType));
    writer.writeUnsignedAt<std::uint8_t>(PageHeaderLayout::Reserved, 0);
    writer.writeUnsignedAt<std::uint16_t>(PageHeaderLayout::SlotCount, header.slotCount);
    writer.writeUnsignedAt<std::uint16_t>(PageHeaderLayout::FreeSpaceStart, header.freeSpaceStart);
    writer.writeUnsignedAt<std::uint16_t>(PageHeaderLayout::FreeSpaceEnd, header.freeSpaceEnd);
    writer.writeUnsignedAt<std::uint32_t>(PageHeaderLayout::NextPageId, header.nextPageId);
}

void PageHeaderWriter::initializeDataPage(std::uint32_t pageId)
{
    PageHeader header{
        .pageId = pageId,
        .pageType = PageType::DataPage,
        .slotCount = 0,
        .freeSpaceStart = static_cast<std::uint16_t>(PageHeaderLayout::Size),
        .freeSpaceEnd = static_cast<std::uint16_t>(PAGE_SIZE),
        .nextPageId = 0};

    write(header);
}

void PageHeaderWriter::setSlotCount(std::uint16_t slotCount)
{
    writer.writeUnsignedAt<std::uint16_t>(PageHeaderLayout::SlotCount, slotCount);
}

void PageHeaderWriter::setFreeSpaceStart(std::uint16_t freeSpaceStart)
{
    writer.writeUnsignedAt<std::uint16_t>(PageHeaderLayout::FreeSpaceStart, freeSpaceStart);
}

void PageHeaderWriter::setFreeSpaceEnd(std::uint16_t freeSpaceEnd)
{
    writer.writeUnsignedAt<std::uint16_t>(PageHeaderLayout::FreeSpaceEnd, freeSpaceEnd);
}

void PageHeaderWriter::setNextPageId(std::uint32_t nextPageId)
{
    writer.writeUnsignedAt<std::uint32_t>(PageHeaderLayout::NextPageId, nextPageId);
}

HeaderPageWriter::HeaderPageWriter(PageWriter &writer)
    : writer(writer) {}

void HeaderPageWriter::write(const HeaderPage &header)
{
    writer.writeString(header.magic);
    writer.writeUnsigned<std::uint16_t>(header.version);
    writer.writeUnsigned<std::uint32_t>(header.pageSize);
    writer.writeString(header.tableName);

    writer.writeUnsigned<std::uint32_t>(static_cast<std::uint32_t>(header.columns.size()));

    for (const Column &column : header.columns)
    {
        writeColumn(column);
    }

    writer.writeUnsigned<std::uint64_t>(header.totalRowCount);
    writer.writeUnsigned<std::uint32_t>(header.firstDataPageId);
    writer.writeUnsigned<std::uint32_t>(header.lastDataPageId);
    writer.writeUnsigned<std::uint32_t>(header.nextUnusedPageId);
}

void HeaderPageWriter::writeColumn(const Column &column)
{
    writer.writeString(column.name);
    writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(column.type));
    writer.writeUnsigned<std::uint8_t>(column.nullable ? 1 : 0);
    writer.writeUnsigned<std::uint32_t>(column.columnIndex);
    writeColumnStorage(column.storage);
}

void HeaderPageWriter::writeColumnStorage(const ColumnStorage &storage)
{
    if (std::holds_alternative<FixedColumnStorage>(storage))
    {
        const FixedColumnStorage &fixed = std::get<FixedColumnStorage>(storage);

        writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(ColumnStorageKind::Fixed));
        writer.writeUnsigned<std::uint32_t>(fixed.offset);
        writer.writeUnsigned<std::uint32_t>(fixed.size);
        return;
    }

    if (std::holds_alternative<VarColumnStorage>(storage))
    {
        const VarColumnStorage &var = std::get<VarColumnStorage>(storage);

        writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(ColumnStorageKind::Variable));
        writer.writeUnsigned<std::uint32_t>(var.varIndex);
        return;
    }

    throw std::runtime_error("Unknown column storage type");
}

DataPageWriter::DataPageWriter(PageWriter &writer, const HeaderPage &tableHeader)
    : writer(writer),
      headerWriter(writer),
      slotWriter(writer),
      tableHeader(tableHeader) {}

void DataPageWriter::write(std::uint32_t pageId, const std::vector<Row> &rows)
{
    PageHeader header{
        .pageId = pageId,
        .pageType = PageType::DataPage,
        .slotCount = 0,
        .freeSpaceStart = static_cast<std::uint16_t>(PageHeaderLayout::Size),
        .freeSpaceEnd = static_cast<std::uint16_t>(PAGE_SIZE),
        .nextPageId = 0};

    write(header, rows);
}

void DataPageWriter::write(const PageHeader &pageHeader, const std::vector<Row> &rows)
{
    PageHeader header = pageHeader;
    header.pageType = PageType::DataPage;
    header.slotCount = 0;
    header.freeSpaceStart = static_cast<std::uint16_t>(PageHeaderLayout::Size);
    header.freeSpaceEnd = static_cast<std::uint16_t>(PAGE_SIZE);

    headerWriter.write(header);

    std::uint16_t freeStart = static_cast<std::uint16_t>(PageHeaderLayout::Size);
    std::uint16_t freeEnd = static_cast<std::uint16_t>(PAGE_SIZE);
    std::uint16_t slotIndex = 0;

    for (const Row &row : rows)
    {
        std::size_t rowSize = computeRowSize(row.values);

        if (rowSize > std::numeric_limits<std::uint16_t>::max())
        {
            throw std::runtime_error("row too large for uint16_t slot size");
        }

        if (freeStart + rowSize + SlotWriter::SlotSize > freeEnd)
        {
            throw std::runtime_error("not enough space in data page");
        }

        std::uint16_t rowOffset = freeStart;

        writer.seek(rowOffset);

        RowWriter rowWriter(writer, tableHeader);
        rowWriter.writeRow(row.values);

        std::size_t actualRowEnd = writer.position();
        std::size_t expectedRowEnd = static_cast<std::size_t>(rowOffset) + rowSize;

        if (actualRowEnd != expectedRowEnd)
        {
            throw std::runtime_error("computed row size does not match written row size");
        }

        Slot slot{
            .offset = rowOffset,
            .size = static_cast<std::uint16_t>(rowSize),
            .flags = 0};

        slotWriter.writeSlot(slotIndex, slot);

        freeStart = static_cast<std::uint16_t>(expectedRowEnd);
        freeEnd -= static_cast<std::uint16_t>(SlotWriter::SlotSize);

        ++slotIndex;
    }

    headerWriter.setSlotCount(slotIndex);
    headerWriter.setFreeSpaceStart(freeStart);
    headerWriter.setFreeSpaceEnd(freeEnd);
}

std::size_t DataPageWriter::computeRowSize(const std::vector<Value> &values) const
{
    return computeSerializedRowSize(tableHeader, values);
}

RawPage encodeHeaderPage(const PageHeader &pageHeader, const HeaderPage &headerPage)
{
    RawPage rawPage{};
    PageWriter writer(rawPage);

    PageHeaderWriter pageHeaderWriter(writer);
    pageHeaderWriter.write(pageHeader);

    writer.seek(PageHeaderLayout::Size);

    HeaderPageWriter headerPageWriter(writer);
    headerPageWriter.write(headerPage);

    return rawPage;
}

RawPage encodeDataPage(
    const PageHeader &pageHeader,
    const HeaderPage &tableHeader,
    const std::vector<Row> &rows)
{
    RawPage rawPage{};
    PageWriter writer(rawPage);

    DataPageWriter dataPageWriter(writer, tableHeader);
    dataPageWriter.write(pageHeader, rows);

    return rawPage;
}

RawPage encodeDataPage(
    std::uint32_t pageId,
    const HeaderPage &tableHeader,
    const std::vector<Row> &rows)
{
    RawPage rawPage{};
    PageWriter writer(rawPage);

    DataPageWriter dataPageWriter(writer, tableHeader);
    dataPageWriter.write(pageId, rows);

    return rawPage;
}

RawPage encodePage(const Page &page)
{
    if (page.header.pageType == PageType::HeaderPage)
    {
        return encodeHeaderPage(page.header, std::get<HeaderPage>(page.data));
    }

    throw std::runtime_error("encoding data pages requires table header; use encodeDataPage");
}

std::uint16_t calculateFreeSpaceStart(const PageHeader &pageHeader)
{
    std::size_t freeSpaceStart =
        PageHeaderLayout::Size +
        encodedSlotSize() * static_cast<std::size_t>(pageHeader.slotCount);

    if (freeSpaceStart > PAGE_SIZE)
    {
        throw std::runtime_error("Slots exceed page size");
    }

    if (freeSpaceStart > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::runtime_error("freeSpaceStart too large for uint16_t");
    }

    return static_cast<std::uint16_t>(freeSpaceStart);
}

std::size_t encodedSlotSize()
{
    return SlotWriter::SlotSize;
}

std::size_t encodedRowSize(const HeaderPage &tableHeader, const Row &row)
{
    return computeSerializedRowSize(tableHeader, row.values);
}

void writePageHeader(std::vector<std::byte> &buffer, const PageHeader &header)
{
    appendUnsigned<std::uint32_t>(buffer, header.pageId);
    appendUnsigned<std::uint8_t>(buffer, static_cast<std::uint8_t>(header.pageType));
    appendUnsigned<std::uint8_t>(buffer, 0);
    appendUnsigned<std::uint16_t>(buffer, header.slotCount);
    appendUnsigned<std::uint16_t>(buffer, header.freeSpaceStart);
    appendUnsigned<std::uint16_t>(buffer, header.freeSpaceEnd);
    appendUnsigned<std::uint32_t>(buffer, header.nextPageId);
}

void writeHeaderPage(std::vector<std::byte> &buffer, const HeaderPage &header)
{
    appendString(buffer, header.magic);
    appendUnsigned<std::uint16_t>(buffer, header.version);
    appendUnsigned<std::uint32_t>(buffer, header.pageSize);
    appendString(buffer, header.tableName);
    appendUnsigned<std::uint32_t>(buffer, static_cast<std::uint32_t>(header.columns.size()));

    for (const Column &column : header.columns)
    {
        appendString(buffer, column.name);
        appendUnsigned<std::uint8_t>(buffer, static_cast<std::uint8_t>(column.type));
        appendUnsigned<std::uint8_t>(buffer, column.nullable ? 1 : 0);
        appendUnsigned<std::uint32_t>(buffer, column.columnIndex);

        if (std::holds_alternative<FixedColumnStorage>(column.storage))
        {
            const FixedColumnStorage &fixed = std::get<FixedColumnStorage>(column.storage);
            appendUnsigned<std::uint8_t>(buffer, static_cast<std::uint8_t>(ColumnStorageKind::Fixed));
            appendUnsigned<std::uint32_t>(buffer, fixed.offset);
            appendUnsigned<std::uint32_t>(buffer, fixed.size);
        }
        else if (std::holds_alternative<VarColumnStorage>(column.storage))
        {
            const VarColumnStorage &var = std::get<VarColumnStorage>(column.storage);
            appendUnsigned<std::uint8_t>(buffer, static_cast<std::uint8_t>(ColumnStorageKind::Variable));
            appendUnsigned<std::uint32_t>(buffer, var.varIndex);
        }
        else
        {
            throw std::runtime_error("Unknown column storage type");
        }
    }

    appendUnsigned<std::uint64_t>(buffer, header.totalRowCount);
    appendUnsigned<std::uint32_t>(buffer, header.firstDataPageId);
    appendUnsigned<std::uint32_t>(buffer, header.lastDataPageId);
    appendUnsigned<std::uint32_t>(buffer, header.nextUnusedPageId);
}

void writeSlot(std::vector<std::byte> &buffer, const Slot &slot)
{
    appendUnsigned<std::uint16_t>(buffer, slot.offset);
    appendUnsigned<std::uint16_t>(buffer, slot.size);
    appendUnsigned<std::uint16_t>(buffer, slot.flags);
}

std::size_t encodedRowSize(const Row &row)
{
    return encodeRowPayload(row).size();
}

std::vector<std::byte> encodeRowPayload(const Row &row)
{
    std::vector<std::byte> buffer;
    appendUnsigned<std::uint32_t>(buffer, static_cast<std::uint32_t>(row.values.size()));

    for (const Value &value : row.values)
    {
        if (std::holds_alternative<std::monostate>(value))
        {
            appendUnsigned<std::uint8_t>(buffer, static_cast<std::uint8_t>(DataType::Null));
        }
        else if (std::holds_alternative<int>(value))
        {
            appendUnsigned<std::uint8_t>(buffer, static_cast<std::uint8_t>(DataType::Int));
            appendUnsigned<std::uint32_t>(
                buffer,
                static_cast<std::uint32_t>(std::get<int>(value)));
        }
        else if (std::holds_alternative<std::string>(value))
        {
            appendUnsigned<std::uint8_t>(buffer, static_cast<std::uint8_t>(DataType::Text));
            appendString(buffer, std::get<std::string>(value));
        }
    }

    return buffer;
}
