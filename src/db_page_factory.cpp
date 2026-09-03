#include "db_page_factory.h"

#include "db_write.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    std::vector<Column> buildStoredColumns(const std::vector<Column> &columns)
    {
        std::vector<Column> storedColumns;
        storedColumns.reserve(columns.size());

        std::uint32_t fixedOffset = 0;
        std::uint32_t variableIndex = 0;

        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            Column column = columns[i];
            column.columnIndex = static_cast<std::uint32_t>(i);

            switch (column.type)
            {
            case DataType::Int:
                column.storage = FixedColumnStorage{
                    .offset = fixedOffset,
                    .size = static_cast<std::uint32_t>(sizeof(std::int32_t))};
                fixedOffset += static_cast<std::uint32_t>(sizeof(std::int32_t));
                break;

            case DataType::BigInt:
                column.storage = FixedColumnStorage{
                    .offset = fixedOffset,
                    .size = static_cast<std::uint32_t>(sizeof(std::int64_t))};
                fixedOffset += static_cast<std::uint32_t>(sizeof(std::int64_t));
                break;

            case DataType::Double:
                column.storage = FixedColumnStorage{
                    .offset = fixedOffset,
                    .size = static_cast<std::uint32_t>(sizeof(std::float64_t))};
                fixedOffset += static_cast<std::uint32_t>(sizeof(std::float64_t));
                break;

            case DataType::Decimal:
                column.storage = FixedColumnStorage{
                    .offset = fixedOffset,
                    .size = static_cast<std::uint32_t>(
                        sizeof(std::int64_t) + sizeof(std::uint32_t))};
                fixedOffset += static_cast<std::uint32_t>(
                    sizeof(std::int64_t) + sizeof(std::uint32_t));
                break;

            case DataType::Boolean:
                column.storage = FixedColumnStorage{
                    .offset = fixedOffset,
                    .size = static_cast<std::uint32_t>(sizeof(std::uint8_t))};
                fixedOffset += static_cast<std::uint32_t>(sizeof(std::uint8_t));
                break;

            case DataType::Text:
                column.storage = VarColumnStorage{.varIndex = variableIndex};
                ++variableIndex;
                break;

            case DataType::Null:
                column.storage = FixedColumnStorage{
                    .offset = fixedOffset,
                    .size = 0};
                break;

            default:
                throw std::runtime_error("Unsupported column type in table schema");
            }

            storedColumns.push_back(std::move(column));
        }

        return storedColumns;
    }

    std::uint16_t calculateHeaderFreeSpaceStart(
        const PageHeader &pageHeader,
        const HeaderPage &headerPage)
    {
        RawPage rawPage{};
        PageWriter writer(rawPage);

        PageHeaderWriter pageHeaderWriter(writer);
        pageHeaderWriter.write(pageHeader);

        writer.seek(PageHeaderLayout::Size);

        HeaderPageWriter headerPageWriter(writer);
        headerPageWriter.write(headerPage);

        std::size_t freeSpaceStart = writer.position();

        if (freeSpaceStart > std::numeric_limits<std::uint16_t>::max())
        {
            throw std::runtime_error(
                "Header page freeSpaceStart too large for uint16_t");
        }

        return static_cast<std::uint16_t>(freeSpaceStart);
    }
}

Page makeEmptyDataPage(std::uint32_t pageId)
{
    DataPage dataPage{
        .slots = {},
        .rows = {}};

    PageHeader pageHeader{
        .pageId = pageId,
        .pageType = PageType::DataPage,
        .slotCount = 0,
        .freeSpaceStart = static_cast<std::uint16_t>(PageHeaderLayout::Size),
        .freeSpaceEnd = static_cast<std::uint16_t>(PAGE_SIZE),
        .nextPageId = 0};

    return Page{
        .header = pageHeader,
        .data = std::move(dataPage)};
}

Page makeHeaderPage(
    const std::string &name,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<Constraint> &constraints)
{
    PageHeader pageHeader{
        .pageId = 0,
        .pageType = PageType::HeaderPage,
        .slotCount = 0,
        .freeSpaceStart = 0,
        .freeSpaceEnd = static_cast<std::uint16_t>(PAGE_SIZE),
        .nextPageId = 0};

    HeaderPage tableHeader{
        .magic = magic,
        .version = 1,
        .pageSize = PAGE_SIZE,
        .tableName = name,
        .columns = buildStoredColumns(columns),
        .constraints = constraints,
        .totalRowCount = 0,
        .firstDataPageId = 0,
        .lastDataPageId = 0,
        .nextUnusedPageId = 1};

    pageHeader.freeSpaceStart =
        calculateHeaderFreeSpaceStart(pageHeader, tableHeader);

    return Page{
        .header = pageHeader,
        .data = std::move(tableHeader)};
}
