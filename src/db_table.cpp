#include "db_table.h"

#include "db_expression_evaluator.h"
#include "db_page_factory.h"
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
}

TableCursor::TableCursor(TableCursor &&other) noexcept
    : table_(std::exchange(other.table_, nullptr)),
      pageId_(std::exchange(other.pageId_, 0)),
      rowIndex_(std::exchange(other.rowIndex_, 0)),
      currentPage_(std::move(other.currentPage_)),
      seenPageIds_(std::move(other.seenPageIds_))
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
        seenPageIds_ = std::move(other.seenPageIds_);
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
            if (!seenPageIds_.insert(pageId_).second)
            {
                throw std::runtime_error(
                    "Detected a cycle at page " + std::to_string(pageId_));
            }
            currentPage_.emplace(table_->bufferManager.getDataPage(pageId_));
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
                .row = row};
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
            if (!seenPageIds_.insert(pageId_).second)
            {
                throw std::runtime_error(
                    "Detected a cycle at page " + std::to_string(pageId_));
            }
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
    const LinuxDirectory &tablesDir,
    const std::string &tableName,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<BoundConstraint> &constraints,
    StatementRecovery &statementRecovery)
{
    Table table{tablesDir, tableName + ".table", LinuxFile::OpenMode::CreateNew, statementRecovery};
    table.initializeNewTable(tableName, magic, columns, constraints);
    return table;
}

Table Table::open(const LinuxDirectory &tablesDir,
                  const std::filesystem::path &relTablePath, StatementRecovery &statementRecovery)
{
    Table table{tablesDir, relTablePath, LinuxFile::OpenMode::OpenExisting, statementRecovery};
    table.validateHeaderPage();
    return table;
}

void Table::insertRows(const BoundInsert &insert)
{
    bufferManager.requireReadable();
    if (insert.rows.empty())
    {
        return;
    }
    bufferManager.insertRows(insert.rows);
    bufferManager.flushAll();
    bufferManager.sync();
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

Table::Table(const LinuxDirectory &tablesDir, const std::filesystem::path &relTablePath, LinuxFile::OpenMode mode, StatementRecovery &statementRecovery)
    : bufferManager(tablesDir, relTablePath, mode, statementRecovery)
{
}

void Table::initializeNewTable(
    const std::string &tableName,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<BoundConstraint> &constraints)
{
    bufferManager.initializeNewTable(tableName, magic, columns, constraints);
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
