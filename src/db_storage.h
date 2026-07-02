#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <optional>
#include <filesystem>
#include <fstream>
#include <format>

constexpr size_t PAGE_SIZE = 4096;
using Value = std::variant<std::monostate, int, std::string>;

enum class DataType : uint8_t
{
    Null = 0,
    Int = 1,
    Text = 2
};

// struct Value
// {
//     DataType dataType;
//     uint8_t isNull;
//     ValueData value;
// };
struct FixedColumnStorage
{
    uint32_t offset;
    uint32_t size;
};

struct VarColumnStorage
{
    uint32_t varIndex;
};

enum class ColumnStorageKind : std::uint8_t
{
    Fixed = 1,
    Variable = 2
};

using ColumnStorage = std::variant<FixedColumnStorage, VarColumnStorage>;

struct Column
{
    std::string name;
    DataType type;
    bool nullable;
    uint32_t columnIndex;
    ColumnStorage storage;
};

struct Row
{
    std::vector<Value> values;
};

struct RowValidationResult
{
    bool valid;
    std::optional<size_t> columnIndex;
    std::string message;
};

RowValidationResult validateRowAgainstSchema(const std::vector<Column> &columns, const Row &row);

enum class PageType : uint8_t
{
    DataPage = 1,
    IndexPage = 2,
    OverflowPage = 3,
    FreePage = 4,
    HeaderPage = 5
};

struct PageHeader
{
    uint32_t pageId;
    PageType pageType;
    uint16_t slotCount;
    uint16_t freeSpaceStart;
    uint16_t freeSpaceEnd;
    uint32_t nextPageId; // 0 could mean "no next page"
};

namespace PageHeaderLayout
{
    constexpr std::size_t PageId = 0;         // uint32
    constexpr std::size_t PageType = 4;       // uint8
    constexpr std::size_t Reserved = 5;       // uint8
    constexpr std::size_t SlotCount = 6;      // uint16
    constexpr std::size_t FreeSpaceStart = 8; // uint16
    constexpr std::size_t FreeSpaceEnd = 10;  // uint16
    constexpr std::size_t NextPageId = 12;    // uint32

    constexpr std::size_t Size = 16;
}

struct HeaderPage
{
    std::string magic = "MYDB";
    uint16_t version = 1;
    uint32_t pageSize = PAGE_SIZE;
    std::string tableName;
    std::vector<Column> columns;

    uint64_t totalRowCount = 0;
    uint32_t firstDataPageId = 1;
    uint32_t lastDataPageId = 1;
    uint32_t nextUnusedPageId = 2;
};
// [PageHeader][slots...][free space...][row payload bytes...]
enum class SlotFlag : std::uint16_t
{
    Deleted = 1u << 0,
    Moved = 1u << 1,
    Overflow = 1u << 2
};

inline std::uint16_t toMask(SlotFlag flag)
{
    return static_cast<std::uint16_t>(flag);
}

struct Slot
{
    std::uint16_t offset;
    std::uint16_t size;
    std::uint16_t flags = 0;

    void set(SlotFlag flag)
    {
        flags |= toMask(flag);
    }

    void clear(SlotFlag flag)
    {
        flags &= static_cast<std::uint16_t>(~toMask(flag));
    }

    bool has(SlotFlag flag) const
    {
        return (flags & toMask(flag)) != 0;
    }
};
struct RowEntry
{
    uint16_t slotIndex;
    Row row;
};

struct DataPage
{
    std::vector<Slot> slots;
    std::vector<RowEntry> rows;
};

// struct DataPage
// {
//     std::vector<Slot> slots;
//     std::vector<Row> rows;
// };

using PageData = std::variant<HeaderPage, DataPage>;

struct Page
{
    PageHeader header;
    PageData data;
};

struct PageFrame
{
    uint32_t pageId;
    Page page;
    bool dirty;
};

struct FreePageHeader
{
    PageType type;              // FreePage
    std::uint32_t nextFreePage; // linked list
};

using RawPage = std::array<std::byte, PAGE_SIZE>;
