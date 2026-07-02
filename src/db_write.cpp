#include <string>
#include <vector>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <filesystem>
#include <fstream>
#include <format>
#include <cmath>
#include "db_write.h"
#include "db_storage.h"
// magic = "MYDB"
// version = 1
// page_size = 4096
// table_name = "users"
// column_count = 2

// 4D 59 44 42      // "MYDB"
// 01 00 00 00      // version = 1
// 00 10 00 00      // page_size = 4096
// 05 00 00 00      // table name length = 5
// 75 73 65 72 73   // "users"
// 02 00 00 00      // column count = 2

// template <typename T>
// void writeUnsigned(std::vector<std::byte> &buffer, T value)
// {
//     static_assert(std::is_unsigned_v<T>, "T must be an unsigned integer type");

//     for (size_t shift = 0; shift < sizeof(T) * 8; shift += 8)
//     {
//         buffer.push_back(
//             static_cast<std::byte>((value >> shift) & 0xFF));
//     }
// }

// void writeUInt8(std::vector<std::byte> &buffer, uint8_t value)
// {
//     writeUnsigned(buffer, value);
// }

// void writeUInt16(std::vector<std::byte> &buffer, uint16_t value)
// {
//     writeUnsigned(buffer, value);
// }

// void writeUInt32(std::vector<std::byte> &buffer, uint32_t value)
// {
//     writeUnsigned(buffer, value);
// }

// void writeUInt64(std::vector<std::byte> &buffer, uint64_t value)
// {
//     writeUnsigned(buffer, value);
// }

// void writeInt8(std::vector<std::byte> &buffer, int8_t value)
// {
//     writeUInt8(buffer, static_cast<uint8_t>(value));
// }

// void writeInt16(std::vector<std::byte> &buffer, int16_t value)
// {
//     writeUInt16(buffer, static_cast<uint16_t>(value));
// }
// void writeInt32(std::vector<std::byte> &buffer, int32_t value)
// {
//     writeUInt32(buffer, static_cast<uint32_t>(value));
// }

// void writeInt64(std::vector<std::byte> &buffer, int64_t value)
// {
//     writeUInt64(buffer, static_cast<uint64_t>(value));
// }

// void writeString(std::vector<std::byte> &buffer, const std::string &value)
// {
//     writeInt32(buffer, static_cast<int32_t>(value.size()));

//     for (char ch : value)
//     {
//         buffer.push_back(static_cast<std::byte>(ch));
//     }
// }

// void writeValue(std::vector<std::byte> &buffer, const Value &value)
// {
//     if (std::holds_alternative<std::monostate>(value))
//     {
//         buffer.push_back(static_cast<std::byte>(DataType::Null));
//     }
//     else if (std::holds_alternative<int32_t>(value))
//     {
//         buffer.push_back(static_cast<std::byte>(DataType::Int));
//         writeInt32(buffer, std::get<int32_t>(value));
//     }
//     else if (std::holds_alternative<std::string>(value))
//     {
//         buffer.push_back(static_cast<std::byte>(DataType::Text));
//         writeString(buffer, std::get<std::string>(value));
//     }
// }

// my_database/
// ├── users.table
// ├── products.table
// ├── indexes/
// │   └── users_id.index
// └── wal.log

// [row size][value type][value bytes][value type][value bytes]

// size_t offset = pageId * PAGE_SIZE;
// file.seekg(offset);

// page id
// page type
// checksum
// log sequence number
// free space pointer
// slot count
// previous page pointer
// next page pointer
// lower/upper free space boundaries
// flags

void writeRow(std::vector<std::byte> &buffer, const Row &row)
{
    // Store how many values/columns this row has
    writeInt32(buffer, static_cast<int32_t>(row.values.size()));

    // Store each value
    for (const Value &value : row.values)
    {
        writeValue(buffer, value);
    }
}

// void createTable(const std::filesystem::path &dbRootPath, const std::string &tableName, const TableHeader &tableheader)
// {
//     std::fstream file = openOrCreateFile(dbRootPath / "tables" / (tableName + ".table"));
//     if (!file)
//     {
//         std::cout << "Failed to open file\n";
//         return;
//     }
//     std::vector<std::byte> rowBytes = encodeRow(row);
//     file.write(
//         reinterpret_cast<const char *>(rowBytes.data()),
//         static_cast<std::streamsize>(rowBytes.size()));
// }

class PageHeaderWriter
{
public:
    explicit PageHeaderWriter(PageWriter &writer)
        : writer(writer) {}

    void write(const PageHeader &header)
    {

        writer.writeUnsignedAt<uint32_t>(PageHeaderLayout::PageId, header.pageId);

        writer.writeUnsignedAt<uint8_t>(
            PageHeaderLayout::PageType,
            static_cast<std::uint8_t>(header.pageType));

        writer.writeUnsignedAt<uint8_t>(PageHeaderLayout::Reserved, 0);

        writer.writeUnsignedAt<uint16_t>(PageHeaderLayout::SlotCount, header.slotCount);
        writer.writeUnsignedAt<uint16_t>(PageHeaderLayout::FreeSpaceStart, header.freeSpaceStart);
        writer.writeUnsignedAt<uint16_t>(PageHeaderLayout::FreeSpaceEnd, header.freeSpaceEnd);
        writer.writeUnsignedAt<uint32_t>(PageHeaderLayout::NextPageId, header.nextPageId);
    }

    void initializeDataPage(std::uint32_t pageId)
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

    void setSlotCount(std::uint16_t slotCount)
    {
        writer.writeUnsignedAt<uint16_t>(PageHeaderLayout::SlotCount, slotCount);
    }

    void setFreeSpaceStart(std::uint16_t freeSpaceStart)
    {
        writer.writeUnsignedAt<uint16_t>(PageHeaderLayout::FreeSpaceStart, freeSpaceStart);
    }

    void setFreeSpaceEnd(std::uint16_t freeSpaceEnd)
    {
        writer.writeUnsignedAt<uint16_t>(PageHeaderLayout::FreeSpaceEnd, freeSpaceEnd);
    }

    void setNextPageId(std::uint32_t nextPageId)
    {
        writer.writeUnsignedAt<uint32_t>(PageHeaderLayout::NextPageId, nextPageId);
    }

private:
    PageWriter &writer;
};

// void writePageHeader(std::vector<std::byte> &buffer, const PageHeader &header)
// {
//     writeUInt32(buffer, header.pageId);
//     buffer.push_back(static_cast<std::byte>(header.pageType));
//     writeUInt16(buffer, header.slotCount);
//     writeUInt16(buffer, header.freeSpaceStart);
//     writeUInt16(buffer, header.freeSpaceEnd);
//     writeUInt32(buffer, header.nextPageId);
// }

// void writeSlot(std::vector<std::byte> &buffer, const Slot &slot)
// {
//     writeUInt16(buffer, slot.offset);
//     writeUInt16(buffer, slot.size);
//     writeUInt8(buffer, slot.deleted);
// }

class DataPageWriter
{
public:
    DataPageWriter(PageWriter &writer, const HeaderPage &tableHeader)
        : writer(writer),
          headerWriter(writer),
          slotWriter(writer),
          tableHeader(tableHeader) {}

    void write(std::uint32_t pageId, const std::vector<Row> &rows)
    {
        headerWriter.initializeDataPage(pageId);

        std::uint16_t freeStart =
            static_cast<std::uint16_t>(PageHeaderLayout::Size);

        std::uint16_t freeEnd =
            static_cast<std::uint16_t>(PAGE_SIZE);

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

private:
    PageWriter &writer;
    PageHeaderWriter headerWriter;
    SlotWriter slotWriter;
    const HeaderPage &tableHeader;

    static constexpr std::size_t VarEntrySize = 8; // uint32 offset + uint32 length

    std::size_t computeRowSize(const std::vector<Value> &values) const
    {
        if (values.size() != tableHeader.columns.size())
        {
            throw std::runtime_error("value count does not match column count");
        }

        std::size_t nullBitmapSize =
            (tableHeader.columns.size() + 7) / 8;

        std::size_t fixedAreaSize = 0;
        std::size_t varColumnCount = 0;
        std::size_t varDataSize = 0;

        for (const Column &column : tableHeader.columns)
        {
            const Value &value = values[column.columnIndex];

            if (std::holds_alternative<FixedColumnStorage>(column.storage))
            {
                const auto &fixed =
                    std::get<FixedColumnStorage>(column.storage);

                fixedAreaSize = std::max<std::size_t>(
                    fixedAreaSize,
                    fixed.offset + fixed.size);
            }
            else if (std::holds_alternative<VarColumnStorage>(column.storage))
            {
                ++varColumnCount;

                if (std::holds_alternative<std::monostate>(value))
                {
                    continue;
                }

                if (column.type == DataType::Text)
                {
                    if (!std::holds_alternative<std::string>(value))
                    {
                        throw std::runtime_error("expected string value for text column: " + column.name);
                    }

                    varDataSize += std::get<std::string>(value).size();
                }
                else
                {
                    throw std::runtime_error("unsupported variable-length column type");
                }
            }
        }

        std::size_t varDirSize = varColumnCount * VarEntrySize;

        return nullBitmapSize + fixedAreaSize + varDirSize + varDataSize;
    }
};

// void writeDataPage(std::vector<std::byte> &buffer, const DataPage &dataPage)
// {
//     // At this point, encodePage already wrote:
//     //
//     // [PageHeader]
//     //
//     // So this function now writes:
//     //
//     // [Slot 0][Slot 1][Slot 2]...[free space]...[row bytes]

//     for (const Slot &slot : dataPage.slots)
//     {
//         writeSlot(buffer, slot);
//     }

//     if (buffer.size() > PAGE_SIZE)
//     {
//         throw std::runtime_error("Page header + slots exceed PAGE_SIZE");
//     }

//     // Make the buffer represent the full page.
//     // The middle becomes zero-filled free space.
//     buffer.resize(PAGE_SIZE, std::byte{0});

//     size_t rowIndex = 0;

//     for (const Slot &slot : dataPage.slots)
//     {
//         if (slot.deleted)
//         {
//             continue;
//         }

//         if (rowIndex >= dataPage.rows.size())
//         {
//             throw std::runtime_error("Not enough rows for active slots");
//         }

//         std::vector<std::byte> rowBuffer;
//         writeRow(rowBuffer, dataPage.rows[rowIndex].row);

//         if (rowBuffer.size() != slot.size)
//         {
//             throw std::runtime_error("Slot size does not match encoded row size");
//         }

//         if (slot.offset + slot.size > PAGE_SIZE)
//         {
//             throw std::runtime_error("Slot points outside page");
//         }

//         std::copy(
//             rowBuffer.begin(),
//             rowBuffer.end(),
//             buffer.begin() + slot.offset);

//         rowIndex++;
//     }

//     if (rowIndex != dataPage.rows.size())
//     {
//         throw std::runtime_error("Too many rows for active slots");
//     }
// }

// void writeColumn(std::vector<std::byte> &buffer, const Column &column)
// {
//     writeString(buffer, column.name);

//     writeUInt8(
//         buffer,
//         static_cast<uint8_t>(column.type));

//     writeUInt8(
//         buffer,
//         column.nullable ? 1 : 0);
// }

// void writeColumnDef(Writer& w, const ColumnDef& col) {
//     w.writeString(col.name);
//     w.writeU8(static_cast<uint8_t>(col.type));
//     w.writeU8(col.nullable ? 1 : 0);
//     w.writeU32(col.columnIndex);

//     if (std::holds_alternative<FixedColumnStorage>(col.storage)) {
//         const auto& fixed = std::get<FixedColumnStorage>(col.storage);

//         w.writeU8(0); // storage kind: fixed
//         w.writeU32(fixed.offset);
//         w.writeU32(fixed.size);
//     } else {
//         const auto& var = std::get<VarColumnStorage>(col.storage);

//         w.writeU8(1); // storage kind: variable
//         w.writeU32(var.varIndex);
//     }
// }

class PageWriter
{
public:
    explicit PageWriter(RawPage &buffer)
        : buffer(buffer) {}

    std::size_t position() const
    {
        return pos;
    }

    void seek(std::size_t newPos)
    {
        if (newPos > PAGE_SIZE)
        {
            throw std::runtime_error("seek past page boundary");
        }

        pos = newPos;
    }
    template <typename T>
    void writeUnsigned(T value)
    {
        static_assert(std::is_unsigned_v<T>, "T must be an unsigned integer type");
        ensureCapacity(sizeof(T));
        for (size_t shift = 0; shift < sizeof(T) * 8; shift += 8)
        {
            buffer[pos++] = (static_cast<std::byte>((value >> shift) & 0xFF));
        }
    }

    template <typename T>
    void writeUnsignedAt(T value, size_t offset)
    {
        std::size_t saved = pos;
        seek(offset);
        writeUnsigned<T>(value);
        seek(saved);
    }

    void writeBytes(const void *data, std::size_t size)
    {
        ensureCapacity(size);

        const auto *bytes = static_cast<const std::byte *>(data);

        std::copy(bytes, bytes + size, buffer.begin() + pos);
        pos += size;
    }

    void writeBytesAt(std::size_t offset, const void *data, std::size_t size)
    {
        std::size_t saved = pos;
        seek(offset);
        writeBytes(data, size);
        seek(saved);
    }

    void writeString(const std::string &value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error("String too large to write");
        }

        writeUnsigned<uint32_t>(static_cast<std::uint32_t>(value.size()));
        writeBytes(value.data(), value.size());
    }

private:
    RawPage &buffer;
    std::size_t pos = 0;

    void ensureCapacity(std::size_t size) const
    {
        if (pos + size > PAGE_SIZE)
        {
            throw std::runtime_error("write past page boundary");
        }
    }
};

class RowWriter
{
public:
    RowWriter(PageWriter &writer, const HeaderPage &headerPage)
        : writer(writer), headerPage(headerPage) {}

    void writeRow(const std::vector<Value> &values)
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

        // Reserve/write null bitmap.
        writer.writeBytes(nullBitmap.data(), nullBitmap.size());

        // Reserve fixed area and varlen directory with zeros.
        std::vector<std::byte> zeros(fixedAreaSize + varDirSize, std::byte{0});
        writer.writeBytes(zeros.data(), zeros.size());

        // Write fixed-width values into fixed area.
        writeFixedValues(values);

        // Move writer to start of varlen data area.
        writer.seek(varDataPos);

        // Write variable-width values and fill varlen directory.
        writeVariableValues(values);

        // Write final null bitmap back at the start of the row.
        writer.writeBytesAt(rowStart, nullBitmap.data(), nullBitmap.size());

        // Leave writer at end of row.
        writer.seek(varDataPos);
    }

private:
    PageWriter &writer;
    const HeaderPage &headerPage;

    std::size_t rowStart = 0;

    std::size_t nullBitmapSizeBytes = 0;
    std::size_t fixedAreaSize = 0;
    std::size_t varDirSize = 0;

    std::size_t fixedAreaStartOffset = 0;
    std::size_t varDirStartOffset = 0;
    std::size_t varDataStartOffset = 0;

    std::size_t varDataPos = 0;

    static constexpr std::size_t VarEntrySize = 8; // uint32 offset + uint32 length

    static void setBitmapBit(std::vector<std::uint8_t> &bitmap, std::size_t bitIndex)
    {
        bitmap[bitIndex / 8] |= static_cast<std::uint8_t>(1u << (bitIndex % 8));
    }

    void prepareLayout()
    {
        nullBitmapSizeBytes = (headerPage.columns.size() + 7) / 8;

        fixedAreaSize = 0;
        std::size_t varCount = 0;

        for (const Column &column : headerPage.columns)
        {
            if (std::holds_alternative<FixedColumnStorage>(column.storage))
            {
                const auto &fixed = std::get<FixedColumnStorage>(column.storage);
                fixedAreaSize = std::max<std::size_t>(
                    fixedAreaSize,
                    fixed.offset + fixed.size);
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

    void writeFixedValues(const std::vector<Value> &values)
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

            const auto &fixed = std::get<FixedColumnStorage>(column.storage);
            writeFixedValue(column, fixed, value);
        }
    }

    void writeFixedValue(
        const Column &column,
        const FixedColumnStorage &fixed,
        const Value &value)
    {
        std::size_t absoluteOffset =
            rowStart + fixedAreaStartOffset + fixed.offset;

        ValueSerializer::writeFixed(
            writer,
            absoluteOffset,
            column.type,
            value);
    }
    void writeVariableValues(const std::vector<Value> &values)
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
                // Directory entry stays zero.
                continue;
            }

            const auto &var = std::get<VarColumnStorage>(column.storage);
            writeVariableValue(column, var, value);
        }
    }

    void writeVariableValue(
        const Column &column,
        const VarColumnStorage &var,
        const Value &value)
    {
        std::vector<std::byte> bytes =
            ValueSerializer::serializeVariable(column.type, value);

        std::uint32_t relativeOffset =
            static_cast<std::uint32_t>(varDataPos - rowStart);

        std::uint32_t length =
            static_cast<std::uint32_t>(bytes.size());

        std::size_t varEntryOffset =
            rowStart + varDirStartOffset + var.varIndex * VarEntrySize;

        writer.writeUnsignedAt<uint32_t>(varEntryOffset, relativeOffset);
        writer.writeUnsignedAt<uint32_t>(varEntryOffset + 4, length);

        writer.seek(varDataPos);
        writer.writeBytes(bytes.data(), bytes.size());

        varDataPos = writer.position();
    }
};

class ValueSerializer
{
public:
    static void writeFixed(
        PageWriter &writer,
        std::size_t absoluteOffset,
        DataType type,
        const Value &value)
    {
        switch (type)
        {
        case DataType::Int:
        {
            if (!std::holds_alternative<int>(value))
            {
                throw std::runtime_error("Expected int value");
            }

            writer.writeUnsignedAt<uint32_t>(
                absoluteOffset,
                static_cast<std::uint32_t>(std::get<int>(value)));
            return;
        }

        default:
            throw std::runtime_error("Unsupported fixed-width type");
        }
    }

    static std::vector<std::byte> serializeVariable(
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

            const auto *data =
                reinterpret_cast<const std::byte *>(s.data());

            return std::vector<std::byte>(data, data + s.size());
        }

        default:
            throw std::runtime_error("Unsupported variable-width type");
        }
    }
};

class BitmapWriter
{
public:
    explicit BitmapWriter(std::span<std::uint8_t> bytes)
        : bytes(bytes) {}

    void set(std::size_t bitIndex)
    {
        bytes[bitIndex / 8] |= static_cast<std::uint8_t>(1u << (bitIndex % 8));
    }

    void clear(std::size_t bitIndex)
    {
        bytes[bitIndex / 8] &= static_cast<std::uint8_t>(~(1u << (bitIndex % 8)));
    }

    bool get(std::size_t bitIndex) const
    {
        return (bytes[bitIndex / 8] &
                static_cast<std::uint8_t>(1u << (bitIndex % 8))) != 0;
    }

private:
    std::span<std::uint8_t> bytes;
};

class SlotWriter
{
public:
    static constexpr std::size_t SlotSize = 6; // offset + size + flags

    explicit SlotWriter(PageWriter &writer)
        : writer(writer) {}

    void writeSlot(std::uint16_t slotIndex, const Slot &slot)
    {
        std::size_t base = slotOffset(slotIndex);

        writer.writeUnsignedAt<uint16_t>(base, slot.offset);
        writer.writeUnsignedAt<uint16_t>(base + 2, slot.size);
        writer.writeUnsignedAt<uint16_t>(base + 4, slot.flags);
    }

    void writeSlot(
        std::uint16_t slotIndex,
        std::uint16_t rowOffset,
        std::uint16_t rowSize,
        std::uint16_t flags = 0)
    {
        Slot slot{
            .offset = rowOffset,
            .size = rowSize,
            .flags = flags};

        writeSlot(slotIndex, slot);
    }

    void markDeleted(std::uint16_t slotIndex, Slot slot)
    {
        slot.set(SlotFlag::Deleted);
        writeSlot(slotIndex, slot);
    }

    std::size_t slotOffset(std::uint16_t slotIndex) const
    {
        return PAGE_SIZE - ((static_cast<std::size_t>(slotIndex) + 1) * SlotSize);
    }

private:
    PageWriter &writer;
};

class HeaderPageWriter
{
public:
    explicit HeaderPageWriter(PageWriter &writer)
        : writer(writer) {}

    void write(const HeaderPage &header)
    {
        writer.writeString(header.magic);
        writer.writeUnsigned<uint16_t>(header.version);
        writer.writeUnsigned<uint32_t>(header.pageSize);
        writer.writeString(header.tableName);

        writer.writeUnsigned<uint32_t>(static_cast<std::uint32_t>(header.columns.size()));

        for (const Column &column : header.columns)
        {
            writeColumn(column);
        }

        writer.writeUnsigned<uint64_t>(header.totalRowCount);
        writer.writeUnsigned<uint32_t>(header.firstDataPageId);
        writer.writeUnsigned<uint32_t>(header.lastDataPageId);
        writer.writeUnsigned<uint32_t>(header.nextUnusedPageId);
    }

private:
    PageWriter &writer;

    void writeColumn(const Column &column)
    {
        writer.writeString(column.name);

        writer.writeUnsigned<uint8_t>(static_cast<std::uint8_t>(column.type));
        writer.writeUnsigned<uint8_t>(column.nullable ? 1 : 0);
        writer.writeUnsigned<uint32_t>(column.columnIndex);

        writeColumnStorage(column.storage);
    }

    void writeColumnStorage(const ColumnStorage &storage)
    {
        if (std::holds_alternative<FixedColumnStorage>(storage))
        {
            const FixedColumnStorage &fixed =
                std::get<FixedColumnStorage>(storage);

            writer.writeUnsigned<uint8_t>(static_cast<std::uint8_t>(ColumnStorageKind::Fixed));
            writer.writeUnsigned<uint32_t>(fixed.offset);
            writer.writeUnsigned<uint32_t>(fixed.size);
            return;
        }

        if (std::holds_alternative<VarColumnStorage>(storage))
        {
            const VarColumnStorage &var =
                std::get<VarColumnStorage>(storage);

            writer.writeUnsigned<uint8_t>(static_cast<std::uint8_t>(ColumnStorageKind::Variable));
            writer.writeUnsigned<uint32_t>(var.varIndex);
            return;
        }

        throw std::runtime_error("Unknown column storage type");
    }
};

// void writeHeaderPage(std::vector<std::byte> &buffer, const HeaderPage &header)
// {

//     writeString(buffer, header.magic);
//     writeUInt16(buffer, header.version);
//     writeUInt32(buffer, header.pageSize);
//     writeString(buffer, header.tableName);
//     writeUInt32(buffer, static_cast<uint32_t>(header.columns.size()));

//     // Write column definitions
//     for (const Column &column : header.columns)
//     {
//         writeColumn(buffer, column);
//     }
//     writeUInt64(buffer, header.totalRowCount);
//     writeUInt32(buffer, header.firstDataPageId);
//     writeUInt32(buffer, header.lastDataPageId);
//     writeUInt32(buffer, header.nextUnusedPageId);
// }

// RawPage encodePage(const Page &page)
// {
//     std::vector<std::byte> buffer;
//     buffer.reserve(PAGE_SIZE);

//     writePageHeader(buffer, page.header);

//     switch (page.header.pageType)
//     {
//     case PageType::TableHeader:
//     {
//         const HeaderPage &headerPage = std::get<HeaderPage>(page.data);
//         writeHeaderPage(buffer, headerPage);
//         break;
//     }

//     case PageType::Data:
//     {
//         const DataPage &dataPage = std::get<DataPage>(page.data);
//         writeDataPage(buffer, dataPage);
//         break;
//     }

//     default:
//         throw std::runtime_error("Unsupported page type while encoding page");
//     }

//     if (buffer.size() > PAGE_SIZE)
//     {
//         throw std::runtime_error("Page is larger than PAGE_SIZE");
//     }

//     RawPage pageBuffer{};

//     std::copy(buffer.begin(), buffer.end(), pageBuffer.begin());

//     return pageBuffer;
// }

// uint16_t calculateFreeSpaceStart(const PageHeader &pageHeader)
// {
//     std::vector<std::byte> buffer;

//     writePageHeader(buffer, pageHeader);

//     size_t slotsSize =
//         encodedSlotSize() * static_cast<size_t>(pageHeader.slotCount);

//     size_t freeSpaceStart = buffer.size() + slotsSize;

//     if (freeSpaceStart > PAGE_SIZE)
//     {
//         throw std::runtime_error("Slots exceed page size");
//     }

//     if (freeSpaceStart > std::numeric_limits<uint16_t>::max())
//     {
//         throw std::runtime_error("freeSpaceStart too large for uint16_t");
//     }

//     return static_cast<uint16_t>(freeSpaceStart);
// }

// size_t valueEncodedSize(const Value &value)
// {
//     // Every value has a 1-byte type tag
//     size_t size = sizeof(uint8_t);

//     if (std::holds_alternative<std::monostate>(value))
//     {
//         // NULL only stores the type tag
//         return size;
//     }

//     if (std::holds_alternative<int32_t>(value))
//     {
//         // type tag + int32 bytes
//         return size + sizeof(int32_t);
//     }

//     if (std::holds_alternative<std::string>(value))
//     {
//         const std::string &text = std::get<std::string>(value);

//         // type tag + string length + string bytes
//         return size + sizeof(uint32_t) + text.size();
//     }

//     throw std::runtime_error("Unknown value type");
// }

// size_t rowPayloadSize(const Row &row)
// {
//     size_t size = sizeof(uint32_t); // value_count

//     for (const Value &value : row.values)
//     {
//         size += valueEncodedSize(value);
//     }

//     return size;
// }

// size_t encodedRowSize(const Row &row)
// {
//     return sizeof(uint32_t) + rowPayloadSize(row);
// }

// std::vector<std::byte> encodeRowPayload(const Row &row)
// {
//     std::vector<std::byte> buffer;
//     writeRow(buffer, row); // [value_count][values...]
//     return buffer;
// }
