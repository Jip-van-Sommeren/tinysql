#pragma once

#include "db_buffer_manager.h"
#include "db_storage.h"

#include <filesystem>
#include <string>
#include <vector>

struct BoundInsert;
struct BoundDelete;


class TableCursor
{
public:
    explicit TableCursor(const Table& table)
        : table_(table),
          pageId_(table.firstDataPageId())
    {
    }

    std::optional<Row> next();

private:
    const Table& table_;

    PageId pageId_;
    std::uint32_t slotIndex_ = 0;
};

class Table
{
public:
    static Table create(
        std::filesystem::path tablePath,
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns,
        const std::vector<Constraint> &constraints);

    static Table open(std::filesystem::path tablePath);

    void insertRows(const BoundInsert &insert);
    std::vector<Row> scan();
    TableCursor scan() const
    {
        return TableCursor(*this);
    }
    std::uint64_t deleteRows(const BoundDelete &del);
    PageId firstDataPageId() ;
    PageGuard getPageForScan(PageId pageId) ;

private:
    explicit Table(std::filesystem::path tablePath);

    BufferManager bufferManager;

    void initializeNewTable(
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns,
        const std::vector<Constraint> &constraints);
    void validateHeaderPage();
};
