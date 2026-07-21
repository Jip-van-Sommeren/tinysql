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
#include "db_sql_parser.h"

constexpr size_t PAGE_SIZE = 4096;
using Value = std::variant<std::monostate, int, std::string>;

enum class DataType : uint8_t
{
    Null = 0,
    Int = 1,
    Text = 2
};

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

struct Constraint
{
    std::string name;
    DataType type;
    ConstraintType constraintType;
};
struct Column
{
    std::string name;
    DataType type;
    bool nullable;
    uint32_t columnIndex;
    // Constraint constraint;
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

struct QueryResult
{
    std::vector<Row> rows;
    std::uint64_t affectedRows = 0;
    bool returnsRows = false;
};

struct HeaderPage
{
    std::string magic = "MYDB";
    uint16_t version = 1;
    uint32_t pageSize = PAGE_SIZE;
    std::string tableName;
    std::vector<Column> columns;
    std::vector<Constraint> constraints;

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

struct RawPage
{
    std::array<std::byte, PAGE_SIZE> bytes{};

    constexpr std::size_t size() const noexcept
    {
        return bytes.size();
    }

    std::byte *data() noexcept
    {
        return bytes.data();
    }

    const std::byte *data() const noexcept
    {
        return bytes.data();
    }

    auto begin() noexcept
    {
        return bytes.begin();
    }

    auto begin() const noexcept
    {
        return bytes.begin();
    }

    auto end() noexcept
    {
        return bytes.end();
    }

    auto end() const noexcept
    {
        return bytes.end();
    }

    std::byte &operator[](std::size_t index)
    {
        return bytes[index];
    }

    const std::byte &operator[](std::size_t index) const
    {
        return bytes[index];
    }
};
