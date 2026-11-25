#pragma once

#include "db_buffer_manager.h"
#include "db_storage.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <set>
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
    std::uint64_t remainingPages_ = 0;
};

class Table
{
public:
    static Table create(
        const LinuxDirectory &tablesDir,
        const CreateTablePayload &payload, StatementRecovery &statementRecovery);

    static Table open(const LinuxDirectory &dbRoot, const std::filesystem::path &relTablePath, StatementRecovery &statementRecovery);

    std::vector<LocatedRow> insertRows(const BoundInsert &insert);
    std::vector<LocatedRow> insertRows(const std::vector<Row> &rows);
    // Replacements may move; return their actual stored RowIds.
    std::vector<LocatedRow> updateRows(const std::vector<LocatedRow> &updateRows);

    TableCursor scan() &;
    std::uint64_t deleteRows(const std::vector<LocatedRow> &delRows);
    std::vector<BoundConstraint> getConstraints();
    void truncateTable();
    void renameTable(const std::string &newTableName);
    void renameColumn(const ColumnId colId, const std::string &newName);
    void removeForeignKeysTo(const std::set<std::string> &tableNames);
    void addColumn(const Column &column);
    void addConstraint(const BoundConstraint &constraint);

    void dropColumn(const Column &column);
    // Remap surviving incoming references and remove dependencies with CASCADE.
    void updateForeignKeysForDroppedColumn(
        const std::string &targetTable, ColumnId columnId, bool cascade);
    void dropConstraint(const std::string &constraintName);

private:
    friend class TableCursor;

    explicit Table(const LinuxDirectory &tablesDir,
                   const std::filesystem::path &relTablePath,
                   LinuxFile::OpenMode mode,
                   StatementRecovery &statementRecovery,
                   std::optional<std::vector<Column>> schema);

    BufferManager bufferManager;

    void initializeNewTable(
        const CreateTablePayload &payload);
    void validateHeaderPage();
};
