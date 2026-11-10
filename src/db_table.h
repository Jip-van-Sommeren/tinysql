#pragma once

#include "db_buffer_manager.h"
#include "db_storage.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <unordered_set>

struct BoundInsert;
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
    std::optional<LocatedRow> nextLocated();

private:
    friend class Table;

    explicit TableCursor(Table &table);

    Table *table_;
    PageId pageId_;
    std::size_t rowIndex_ = 0;
    std::optional<PageGuard> currentPage_;
    std::unordered_set<std::uint32_t> seenPageIds_;
};

class Table
{
public:
    static Table create(
        const LinuxDirectory &tablesDir,
        const CreateTablePayload &payload, StatementRecovery &statementRecovery);

    static Table open(const LinuxDirectory &dbRoot, const std::filesystem::path &relTablePath, StatementRecovery &statementRecovery);

    void insertRows(const BoundInsert &insert);
    void updateRows(const std::vector<LocatedRow> &updateRows);

    TableCursor scan() &;
    std::uint64_t deleteRows(const std::vector<LocatedRow> &delRows);
    std::vector<BoundConstraint> getConstraints();

private:
    friend class TableCursor;

    explicit Table(const LinuxDirectory &tablesDir, const std::filesystem::path &relTablePath, LinuxFile::OpenMode mode, StatementRecovery &statementRecovery);

    BufferManager bufferManager;

    void initializeNewTable(
        const CreateTablePayload &payload);
    void validateHeaderPage();
};
