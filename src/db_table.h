#pragma once

#include "db_buffer_manager.h"
#include "db_storage.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct BoundInsert;
struct BoundDelete;
class Table;

// Borrows a table, which must remain alive and unmoved. Mutating the table
// during a scan is unsupported. Each returned row is an independent copy.
class TableCursor
{
public:
    TableCursor(const TableCursor &) = delete;
    TableCursor &operator=(const TableCursor &) = delete;
    TableCursor(TableCursor &&other) noexcept;
    TableCursor &operator=(TableCursor &&other) noexcept;

    std::optional<Row> next();

private:
    friend class Table;

    explicit TableCursor(Table &table);

    Table *table_;
    PageId pageId_;
    std::size_t rowIndex_ = 0;
    std::optional<PageGuard> currentPage_;
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
    TableCursor scan() &;
    std::uint64_t deleteRows(const BoundDelete &del);

private:
    friend class TableCursor;

    explicit Table(std::filesystem::path tablePath);

    BufferManager bufferManager;

    void initializeNewTable(
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns,
        const std::vector<Constraint> &constraints);
    void validateHeaderPage();
};
