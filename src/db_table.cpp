#include "db_table.h"

#include "db_expression_evaluator.h"
#include "db_page_factory.h"
#include "db_query_validator.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

TableCursor::TableCursor(Table &table)
    : table_(&table)
{
    const PageGuard header = table_->bufferManager.getHeaderPage();
    pageId_ = header.as<HeaderPage>().firstDataPageId;
}

TableCursor::TableCursor(TableCursor &&other) noexcept
    : table_(std::exchange(other.table_, nullptr)),
      pageId_(std::exchange(other.pageId_, 0)),
      rowIndex_(std::exchange(other.rowIndex_, 0)),
      currentPage_(std::move(other.currentPage_))
{
    other.currentPage_.reset();
}

TableCursor &TableCursor::operator=(TableCursor &&other) noexcept
{
    if (this != &other)
    {
        currentPage_ = std::move(other.currentPage_);
        other.currentPage_.reset();
        table_ = std::exchange(other.table_, nullptr);
        pageId_ = std::exchange(other.pageId_, 0);
        rowIndex_ = std::exchange(other.rowIndex_, 0);
    }
    return *this;
}

std::optional<Row> TableCursor::next()
{
    while (pageId_ != 0)
    {
        if (!currentPage_)
        {
            currentPage_.emplace(table_->bufferManager.getDataPage(pageId_));
        }

        const DataPage &dataPage = currentPage_->as<DataPage>();
        if (rowIndex_ < dataPage.rows.size())
        {
            Row row = dataPage.rows[rowIndex_].row;
            ++rowIndex_;
            return row;
        }

        pageId_ = currentPage_->page().nextPageId();
        rowIndex_ = 0;
        currentPage_.reset();
    }

    return std::nullopt;
}

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
    bufferManager.insertAllRows(insert.rows);
    bufferManager.flushAll();
}

TableCursor Table::scan() &
{
    return TableCursor(*this);
}

std::uint64_t Table::deleteRows(const BoundDelete &del)
{
    PageGuard header = bufferManager.getHeaderPage();
    HeaderPage &headerPage = header.as<HeaderPage>();
    std::uint64_t deletedCount = 0;

    PageId pageId = headerPage.firstDataPageId;
    while (pageId != 0)
    {
        PageGuard data = bufferManager.getDataPage(pageId);
        DataPage &dataPage = data.as<DataPage>();

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
            data.markDirty();
        }

        pageId = data.page().nextPageId();
    }

    if (deletedCount > headerPage.totalRowCount)
    {
        throw std::runtime_error("Deleted row count exceeds table row count");
    }

    if (deletedCount != 0)
    {
        headerPage.totalRowCount -= deletedCount;
        header.markDirty();
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

    bufferManager.setPage(headerPage, 0);
    bufferManager.setPage(firstDataPage, header.firstDataPageId);
    bufferManager.flushAll();
}

void Table::validateHeaderPage()
{
    bufferManager.getHeaderPage();
}
