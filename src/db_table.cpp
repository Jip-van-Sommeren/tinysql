#include "db_table.h"

#include "db_expression_evaluator.h"
#include "db_query_validator.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>
#include <unordered_set>

TableCursor::TableCursor(Table &table)
    : table_(&table)
{
    const PageGuard header = table_->bufferManager.getHeaderPage();
    pageId_ = header.as<HeaderPage>().firstDataPageId;
    remainingPages_ = table_->bufferManager.pageCount();
}

TableCursor::TableCursor(TableCursor &&other) noexcept
    : table_(std::exchange(other.table_, nullptr)),
      pageId_(std::exchange(other.pageId_, 0)),
      rowIndex_(std::exchange(other.rowIndex_, 0)),
      currentPage_(std::move(other.currentPage_)),
      remainingPages_(std::exchange(other.remainingPages_, 0))
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
        remainingPages_ = std::exchange(other.remainingPages_, 0);
    }
    return *this;
}

std::optional<LocatedRow> TableCursor::nextLocated()
{
    if (table_)
        table_->bufferManager.requireReadable();
    while (pageId_ != 0)
    {
        if (!currentPage_)
        {
            if (remainingPages_ == 0)
            {
                throw std::runtime_error(
                    "Detected a cycle at page " + std::to_string(pageId_));
            }
            --remainingPages_;
            currentPage_.emplace(table_->bufferManager.getPage<DataPage>(pageId_));
        }

        const DataPage &dataPage = currentPage_->as<DataPage>();
        if (rowIndex_ < dataPage.rows.size())
        {
            Row row = dataPage.rows[rowIndex_].row;
            auto slotIndex = dataPage.rows[rowIndex_].slotIndex;
            ++rowIndex_;
            RowId rowId{.pageId = pageId_, .slotIndex = slotIndex};
            return LocatedRow{
                .id = rowId,
                .row = std::move(row)};
        }

        pageId_ = currentPage_->page().nextPageId();
        rowIndex_ = 0;
        currentPage_.reset();
    }

    return std::nullopt;
}

std::optional<Row> TableCursor::next()
{
    if (table_)
        table_->bufferManager.requireReadable();
    while (pageId_ != 0)
    {
        if (!currentPage_)
        {
            if (remainingPages_ == 0)
            {
                throw std::runtime_error(
                    "Detected a cycle at page " + std::to_string(pageId_));
            }
            --remainingPages_;
            currentPage_.emplace(table_->bufferManager.getPage<DataPage>(pageId_));
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
    const LinuxDirectory &tablesDir,
    const CreateTablePayload &payload,
    StatementRecovery &statementRecovery)
{
    Table table{tablesDir, payload.tableName + ".table", LinuxFile::OpenMode::CreateNew, statementRecovery, payload.columns};
    table.initializeNewTable(payload);
    return table;
}

Table Table::open(const LinuxDirectory &tablesDir,
                  const std::filesystem::path &relTablePath, StatementRecovery &statementRecovery)
{
    Table table{tablesDir, relTablePath, LinuxFile::OpenMode::OpenExisting, statementRecovery, std::nullopt};
    table.validateHeaderPage();
    return table;
}

std::vector<LocatedRow> Table::insertRows(const BoundInsert &insert)
{
    bufferManager.requireReadable();
    if (insert.rows.empty())
    {
        return {};
    }
    const auto &locatedRows = bufferManager.insertRows(insert.rows);
    bufferManager.flushAll();
    bufferManager.sync();
    return locatedRows;
}

void Table::updateRows(const std::vector<LocatedRow> &updateRows)
{
    bufferManager.requireReadable();
    if (updateRows.empty())
    {
        return;
    }
    bufferManager.updateRows(updateRows);
    bufferManager.flushAll();
    bufferManager.sync();
}

TableCursor Table::scan() &
{
    return TableCursor(*this);
}

std::uint64_t Table::deleteRows(const std::vector<LocatedRow> &delRows)
{
    bufferManager.requireReadable();
    if (delRows.empty())
    {
        return 0;
    }
    auto rowIds = delRows | std::views::transform(&LocatedRow::id);
    const auto deletedCount = bufferManager.deleteRows(rowIds);
    bufferManager.flushAll();
    bufferManager.sync();

    return deletedCount;
}

Table::Table(const LinuxDirectory &tablesDir,
             const std::filesystem::path &relTablePath,
             LinuxFile::OpenMode mode,
             StatementRecovery &statementRecovery,
             std::optional<std::vector<Column>> schema)
    : bufferManager(tablesDir, relTablePath, mode, statementRecovery, schema)
{
}

void Table::initializeNewTable(
    const CreateTablePayload &payload)
{
    // bufferManager.initializeNewTable(tableName, magic, columns, constraints);
    bufferManager.initializeNewTable(payload);
    bufferManager.flushAll();
    bufferManager.sync();
}

void Table::validateHeaderPage()
{
    bufferManager.getHeaderPage();
}

std::vector<BoundConstraint> Table::getConstraints()
{
    PageGuard header = bufferManager.getHeaderPage();
    const HeaderPage &headerPage = header.as<HeaderPage>();
    return headerPage.constraints;
}
