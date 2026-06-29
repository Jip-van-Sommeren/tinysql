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

struct Column
{
    std::string name;
    DataType type;
    bool nullable;
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
    TableHeader = 1,
    Data = 2,
    Index = 3
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
struct Slot
{
    uint16_t offset;
    uint16_t size;
    uint8_t deleted;
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

using RawPage = std::array<std::byte, PAGE_SIZE>;
