#pragma once

#include "db_buffer_manager.h"
#include "db_storage.h"

#include <filesystem>
#include <string>
#include <vector>

struct BoundInsert;
struct BoundDelete;

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
    std::uint64_t deleteRows(const BoundDelete &del);

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
