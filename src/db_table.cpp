#include "db_table.h"

#include "db_expression_evaluator.h"
#include "db_page_factory.h"
#include "db_query_validator.h"
#include "db_read.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

Table Table::create(
    std::filesystem::path tablePath,
    const std::string &tableName,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<Constraint> &constraints)
{
    Table table{std::move(tablePath)};
    table.initializeNewTable(tableName, magic, columns, constraints);
    return table;
}

Table Table::open(std::filesystem::path tablePath)
{
    Table table{std::move(tablePath)};
    table.validateHeaderPage();
    return table;
}

void Table::insertRows(const BoundInsert &insert)
{
    Page &headerPage = bufferManager.getPage(0, decodeHeaderPage);
    bufferManager.insertAllRows({insert.row}, headerPage);
    bufferManager.flushAll();
}

std::vector<Row> Table::selectAllRows()
{
    Page &headerPage = bufferManager.getPage(0, decodeHeaderPage);
    return selectAllRowsFromPages(headerPage);
}

std::vector<Row> Table::selectRows(const BoundSelect &select)
{
    Page &headerPage = bufferManager.getPage(0, decodeHeaderPage);
    std::vector<Row> rows = selectAllRowsFromPages(headerPage);
    std::vector<Row> result;

    for (const Row &row : rows)
    {
        if (select.where && !evaluatePredicate(*select.where, row))
        {
            continue;
        }

        Row projected;
        for (std::uint32_t index : select.projectedColumnIndexes)
        {
            projected.values.push_back(row.values[index]);
        }

        result.push_back(std::move(projected));
    }

    return result;
}

std::uint64_t Table::deleteRows(const BoundDelete &del)
{
    Page &headerPageContainer = bufferManager.getPage(0, decodeHeaderPage);
    HeaderPage &headerPage =
        std::get<HeaderPage>(headerPageContainer.data);
    std::uint64_t deletedCount = 0;

    std::uint32_t pageId = headerPage.firstDataPageId;
    while (pageId != 0)
    {
        Page &dataPageContainer = bufferManager.getPage(
            pageId,
            [&headerPage](const RawPage &rawPage)
            {
                return decodeDataPage(rawPage, headerPage);
            });
        DataPage &dataPage = std::get<DataPage>(dataPageContainer.data);

        const std::size_t previousRowCount = dataPage.rows.size();
        std::erase_if(
            dataPage.rows,
            [&del, &dataPage, &deletedCount](const RowEntry &entry)
            {
                if (del.where && !evaluatePredicate(*del.where, entry.row))
                {
                    return false;
                }

                dataPage.slots.at(entry.slotIndex).set(SlotFlag::Deleted);
                ++deletedCount;
                return true;
            });

        if (dataPage.rows.size() != previousRowCount)
        {
            bufferManager.markDirty(pageId);
        }

        pageId = dataPageContainer.header.nextPageId;
    }

    if (deletedCount > headerPage.totalRowCount)
    {
        throw std::runtime_error("Deleted row count exceeds table row count");
    }

    if (deletedCount != 0)
    {
        headerPage.totalRowCount -= deletedCount;
        bufferManager.markDirty(headerPageContainer.header.pageId);
    }

    bufferManager.flushAll();
    return deletedCount;
}

Table::Table(std::filesystem::path tablePath)
    : bufferManager(std::move(tablePath))
{
}

void Table::initializeNewTable(
    const std::string &tableName,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<Constraint> &constraints)
{
    Page headerPage = makeHeaderPage(tableName, magic, columns, constraints);
    Page firstDataPage = makeEmptyDataPage(1);

    HeaderPage &header = std::get<HeaderPage>(headerPage.data);
    header.firstDataPageId = 1;
    header.lastDataPageId = 1;
    header.nextUnusedPageId = 2;
    header.totalRowCount = 0;

    std::uint32_t firstDataPageId = header.firstDataPageId;
    bufferManager.setPage(headerPage, 0);
    bufferManager.setPage(firstDataPage, firstDataPageId);
    bufferManager.flushAll();
}

void Table::validateHeaderPage()
{
    Page &headerPage = bufferManager.getPage(0, decodeHeaderPage);

    if (headerPage.header.pageType != PageType::HeaderPage)
    {
        throw std::runtime_error("Page 0 is not a table header page");
    }
}

std::vector<Row> Table::selectAllRowsFromPages(Page &headerPageContainer)
{
    std::vector<Row> result;
    HeaderPage &headerPage =
        std::get<HeaderPage>(headerPageContainer.data);

    std::uint32_t pageId = headerPage.firstDataPageId;
    while (pageId != 0)
    {
        Page &dataPageContainer = bufferManager.getPage(
            pageId,
            [&headerPage](const RawPage &rawPage)
            {
                return decodeDataPage(rawPage, headerPage);
            });
        DataPage &dataPage = std::get<DataPage>(dataPageContainer.data);

        for (const RowEntry &entry : dataPage.rows)
        {
            result.push_back(entry.row);
        }

        pageId = dataPageContainer.header.nextPageId;
    }

    return result;
}
