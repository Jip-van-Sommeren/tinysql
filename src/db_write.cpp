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

template <typename T>
void writeUnsigned(std::vector<std::byte> &buffer, T value)
{
    static_assert(std::is_unsigned_v<T>, "T must be an unsigned integer type");

    for (size_t shift = 0; shift < sizeof(T) * 8; shift += 8)
    {
        buffer.push_back(
            static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

void writeUInt8(std::vector<std::byte> &buffer, uint8_t value)
{
    writeUnsigned(buffer, value);
}

void writeUInt16(std::vector<std::byte> &buffer, uint16_t value)
{
    writeUnsigned(buffer, value);
}

void writeUInt32(std::vector<std::byte> &buffer, uint32_t value)
{
    writeUnsigned(buffer, value);
}

void writeUInt64(std::vector<std::byte> &buffer, uint64_t value)
{
    writeUnsigned(buffer, value);
}

void writeInt8(std::vector<std::byte> &buffer, int8_t value)
{
    writeUInt8(buffer, static_cast<uint8_t>(value));
}

void writeInt16(std::vector<std::byte> &buffer, int16_t value)
{
    writeUInt16(buffer, static_cast<uint16_t>(value));
}
void writeInt32(std::vector<std::byte> &buffer, int32_t value)
{
    writeUInt32(buffer, static_cast<uint32_t>(value));
}

void writeInt64(std::vector<std::byte> &buffer, int64_t value)
{
    writeUInt64(buffer, static_cast<uint64_t>(value));
}

void writeString(std::vector<std::byte> &buffer, const std::string &value)
{
    writeInt32(buffer, static_cast<int32_t>(value.size()));

    for (char ch : value)
    {
        buffer.push_back(static_cast<std::byte>(ch));
    }
}

void writeValue(std::vector<std::byte> &buffer, const Value &value)
{
    if (std::holds_alternative<std::monostate>(value))
    {
        buffer.push_back(static_cast<std::byte>(DataType::Null));
    }
    else if (std::holds_alternative<int32_t>(value))
    {
        buffer.push_back(static_cast<std::byte>(DataType::Int));
        writeInt32(buffer, std::get<int32_t>(value));
    }
    else if (std::holds_alternative<std::string>(value))
    {
        buffer.push_back(static_cast<std::byte>(DataType::Text));
        writeString(buffer, std::get<std::string>(value));
    }
}

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

void writePageHeader(std::vector<std::byte> &buffer, const PageHeader &header)
{
    writeUInt32(buffer, header.pageId);
    buffer.push_back(static_cast<std::byte>(header.pageType));
    writeUInt16(buffer, header.slotCount);
    writeUInt16(buffer, header.freeSpaceStart);
    writeUInt16(buffer, header.freeSpaceEnd);
    writeUInt32(buffer, header.nextPageId);
}

void writeSlot(std::vector<std::byte> &buffer, const Slot &slot)
{
    writeUInt16(buffer, slot.offset);
    writeUInt16(buffer, slot.size);
    writeUInt8(buffer, slot.deleted);
}

void writeDataPage(std::vector<std::byte> &buffer, const DataPage &dataPage)
{
    // At this point, encodePage already wrote:
    //
    // [PageHeader]
    //
    // So this function now writes:
    //
    // [Slot 0][Slot 1][Slot 2]...[free space]...[row bytes]

    for (const Slot &slot : dataPage.slots)
    {
        writeSlot(buffer, slot);
    }

    if (buffer.size() > PAGE_SIZE)
    {
        throw std::runtime_error("Page header + slots exceed PAGE_SIZE");
    }

    // Make the buffer represent the full page.
    // The middle becomes zero-filled free space.
    buffer.resize(PAGE_SIZE, std::byte{0});

    size_t rowIndex = 0;

    for (const Slot &slot : dataPage.slots)
    {
        if (slot.deleted)
        {
            continue;
        }

        if (rowIndex >= dataPage.rows.size())
        {
            throw std::runtime_error("Not enough rows for active slots");
        }

        std::vector<std::byte> rowBuffer;
        writeRow(rowBuffer, dataPage.rows[rowIndex].row);

        if (rowBuffer.size() != slot.size)
        {
            throw std::runtime_error("Slot size does not match encoded row size");
        }

        if (slot.offset + slot.size > PAGE_SIZE)
        {
            throw std::runtime_error("Slot points outside page");
        }

        std::copy(
            rowBuffer.begin(),
            rowBuffer.end(),
            buffer.begin() + slot.offset);

        rowIndex++;
    }

    if (rowIndex != dataPage.rows.size())
    {
        throw std::runtime_error("Too many rows for active slots");
    }
}

void writeColumn(std::vector<std::byte> &buffer, const Column &column)
{
    writeString(buffer, column.name);

    writeUInt8(
        buffer,
        static_cast<uint8_t>(column.type));

    writeUInt8(
        buffer,
        column.nullable ? 1 : 0);
}

void writeHeaderPage(std::vector<std::byte> &buffer, const HeaderPage &header)
{

    writeString(buffer, header.magic);
    writeUInt16(buffer, header.version);
    writeUInt32(buffer, header.pageSize);
    writeString(buffer, header.tableName);
    writeUInt32(buffer, static_cast<uint32_t>(header.columns.size()));

    // Write column definitions
    for (const Column &column : header.columns)
    {
        writeColumn(buffer, column);
    }
    writeUInt64(buffer, header.totalRowCount);
    writeUInt32(buffer, header.firstDataPageId);
    writeUInt32(buffer, header.lastDataPageId);
    writeUInt32(buffer, header.nextUnusedPageId);
}

RawPage encodePage(const Page &page)
{
    std::vector<std::byte> buffer;
    buffer.reserve(PAGE_SIZE);

    writePageHeader(buffer, page.header);

    switch (page.header.pageType)
    {
    case PageType::TableHeader:
    {
        const HeaderPage &headerPage = std::get<HeaderPage>(page.data);
        writeHeaderPage(buffer, headerPage);
        break;
    }

    case PageType::Data:
    {
        const DataPage &dataPage = std::get<DataPage>(page.data);
        writeDataPage(buffer, dataPage);
        break;
    }

    default:
        throw std::runtime_error("Unsupported page type while encoding page");
    }

    if (buffer.size() > PAGE_SIZE)
    {
        throw std::runtime_error("Page is larger than PAGE_SIZE");
    }

    RawPage pageBuffer{};

    std::copy(buffer.begin(), buffer.end(), pageBuffer.begin());

    return pageBuffer;
}

uint16_t calculateFreeSpaceStart(const PageHeader &pageHeader)
{
    std::vector<std::byte> buffer;

    writePageHeader(buffer, pageHeader);

    size_t slotsSize =
        encodedSlotSize() * static_cast<size_t>(pageHeader.slotCount);

    size_t freeSpaceStart = buffer.size() + slotsSize;

    if (freeSpaceStart > PAGE_SIZE)
    {
        throw std::runtime_error("Slots exceed page size");
    }

    if (freeSpaceStart > std::numeric_limits<uint16_t>::max())
    {
        throw std::runtime_error("freeSpaceStart too large for uint16_t");
    }

    return static_cast<uint16_t>(freeSpaceStart);
}

size_t valueEncodedSize(const Value &value)
{
    // Every value has a 1-byte type tag
    size_t size = sizeof(uint8_t);

    if (std::holds_alternative<std::monostate>(value))
    {
        // NULL only stores the type tag
        return size;
    }

    if (std::holds_alternative<int32_t>(value))
    {
        // type tag + int32 bytes
        return size + sizeof(int32_t);
    }

    if (std::holds_alternative<std::string>(value))
    {
        const std::string &text = std::get<std::string>(value);

        // type tag + string length + string bytes
        return size + sizeof(uint32_t) + text.size();
    }

    throw std::runtime_error("Unknown value type");
}

size_t rowPayloadSize(const Row &row)
{
    size_t size = sizeof(uint32_t); // value_count

    for (const Value &value : row.values)
    {
        size += valueEncodedSize(value);
    }

    return size;
}

size_t encodedRowSize(const Row &row)
{
    return sizeof(uint32_t) + rowPayloadSize(row);
}

std::vector<std::byte> encodeRowPayload(const Row &row)
{
    std::vector<std::byte> buffer;
    writeRow(buffer, row); // [value_count][values...]
    return buffer;
}