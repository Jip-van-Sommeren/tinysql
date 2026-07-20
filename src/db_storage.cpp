#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <iostream>
#include <variant>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <cmath>
#include <type_traits>
#include "db_write.h"
#include "db_storage.h"
#include "db_read.h"
#include "db_query_validator.h"
// magic = "MYDB"
// version = 1
// page_size = 4096
// table_name = "users"
// column_count = 2

// 4D 59 44 42      // "MYDB"
// 01 00 00 00      // version = 1
// 00 10 00 00      // page_size = 4096
// 05 00 00 00      // table name length = 5
// 75 73 65 72 73   // "users"
// 02 00 00 00      // column count = 2

namespace
{
    Value evaluateValue(const BoundExpr &expr, const Row &row)
    {
        if (const auto *column = dynamic_cast<const BoundColumnExpr *>(&expr))
        {
            return row.values[column->columnIndex];
        }

        if (const auto *literal = dynamic_cast<const BoundLiteralExpr *>(&expr))
        {
            return literal->value;
        }

        throw std::runtime_error("Expression does not evaluate to a value");
    }

    bool compareValues(ComparisonOp op, const Value &left, const Value &right)
    {
        if (std::holds_alternative<int>(left) && std::holds_alternative<int>(right))
        {
            int leftInt = std::get<int>(left);
            int rightInt = std::get<int>(right);

            switch (op)
            {
            case ComparisonOp::Gt:
                return leftInt > rightInt;
            case ComparisonOp::Ge:
                return leftInt >= rightInt;
            case ComparisonOp::Lt:
                return leftInt < rightInt;
            case ComparisonOp::Le:
                return leftInt <= rightInt;
            case ComparisonOp::Eq:
                return leftInt == rightInt;
            case ComparisonOp::Ne:
                return leftInt != rightInt;
            default:
                throw std::runtime_error("Unsupported comparison operator");
            }
        }

        if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right))
        {
            const std::string &leftStr = std::get<std::string>(left);
            const std::string &rightStr = std::get<std::string>(right);

            switch (op)
            {
            case ComparisonOp::Gt:
                return leftStr > rightStr;
            case ComparisonOp::Ge:
                return leftStr >= rightStr;
            case ComparisonOp::Lt:
                return leftStr < rightStr;
            case ComparisonOp::Le:
                return leftStr <= rightStr;
            case ComparisonOp::Eq:
                return leftStr == rightStr;
            case ComparisonOp::Ne:
                return leftStr != rightStr;
            default:
                throw std::runtime_error("Unsupported comparison operator");
            }
        }

        throw std::runtime_error("Cannot compare values of different types");
    }

    bool evaluatePredicate(const BoundExpr &expr, const Row &row)
    {
        if (const auto *comparison = dynamic_cast<const BoundComparisonExpr *>(&expr))
        {
            Value left = evaluateValue(*comparison->left, row);
            Value right = evaluateValue(*comparison->right, row);

            return compareValues(comparison->op, left, right);
        }

        if (const auto *logical = dynamic_cast<const BoundLogicalExpr *>(&expr))
        {
            if (logical->op == LogicalOp::And)
            {
                return evaluatePredicate(*logical->left, row) &&
                       evaluatePredicate(*logical->right, row);
            }

            return evaluatePredicate(*logical->left, row) ||
                   evaluatePredicate(*logical->right, row);
        }
        if (const auto *isNull = dynamic_cast<const BoundIsNullExpr *>(&expr))
        {
            Value value = evaluateValue(*isNull->operand, row);
            bool result = std::holds_alternative<std::monostate>(value);

            return isNull->negated ? !result : result;
        }

        throw std::runtime_error("WHERE expression does not evaluate to a predicate");
    }
    std::fstream openOrCreateFile(const std::filesystem::path &path)
    {
        if (!std::filesystem::exists(path))
        {
            std::ofstream createFile{path, std::ios::binary};
        }

        std::fstream file{
            path,
            std::ios::in | std::ios::out | std::ios::binary};

        return file;
    }

    bool valueMatchesColumn(const Column &column, const Value &value)
    {
        if (std::holds_alternative<std::monostate>(value))
        {
            return column.nullable;
        }

        switch (column.type)
        {
        case DataType::Int:
            return std::holds_alternative<int>(value);

        case DataType::Text:
            return std::holds_alternative<std::string>(value);

        case DataType::Null:
            return std::holds_alternative<std::monostate>(value);

        default:
            return false;
        }
    }

    std::string dataTypeName(DataType type)
    {
        switch (type)
        {
        case DataType::Null:
            return "null";
        case DataType::Int:
            return "int";
        case DataType::Text:
            return "text";
        default:
            return "unknown";
        }
    }

    std::string valueTypeName(const Value &value)
    {
        if (std::holds_alternative<std::monostate>(value))
        {
            return "null";
        }

        if (std::holds_alternative<int>(value))
        {
            return "int";
        }

        if (std::holds_alternative<std::string>(value))
        {
            return "text";
        }

        return "unknown";
    }

    void printValue(const Value &value)
    {
        std::visit([](const auto &innerValue)
                   {
                       using T = std::decay_t<decltype(innerValue)>;

                       if constexpr (std::is_same_v<T, std::monostate>)
                       {
                           std::cout << "NULL";
                       }
                       else
                       {
                           std::cout << innerValue;
                       } },
                   value);
    }

    void printRows(const std::vector<Row> &rows)
    {
        for (const Row &row : rows)
        {
            for (std::size_t i = 0; i < row.values.size(); ++i)
            {
                if (i != 0)
                {
                    std::cout << "\t";
                }

                printValue(row.values[i]);
            }

            std::cout << "\n";
        }
    }

    RowValidationResult validateRowAgainstSchema(const std::vector<Column> &columns, const Row &row)
    {
        if (columns.size() != row.values.size())
        {
            return RowValidationResult{
                .valid = false,
                .columnIndex = std::nullopt,
                .message = std::format(
                    "row has {} values but schema expects {} columns",
                    row.values.size(),
                    columns.size())};
        }

        for (const Column &column : columns)
        {
            const Value &value = row.values[column.columnIndex];

            if (std::holds_alternative<std::monostate>(value) && !column.nullable)
            {
                return RowValidationResult{
                    .valid = false,
                    .columnIndex = column.columnIndex,
                    .message = std::format(
                        "column '{}' does not allow null values",
                        column.name)};
            }

            if (!valueMatchesColumn(column, value))
            {
                return RowValidationResult{
                    .valid = false,
                    .columnIndex = column.columnIndex,
                    .message = std::format(
                        "column '{}' expects type '{}' but row value has type '{}'",
                        column.name,
                        dataTypeName(column.type),
                        valueTypeName(value))};
            }
        }

        return RowValidationResult{
            .valid = true,
            .columnIndex = std::nullopt,
            .message = ""};
    }

    std::vector<Column> buildStoredColumns(const std::vector<Column> &columns)
    {
        std::vector<Column> storedColumns;
        storedColumns.reserve(columns.size());

        std::uint32_t fixedOffset = 0;
        std::uint32_t varIndex = 0;

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

            case DataType::Text:
                column.storage = VarColumnStorage{.varIndex = varIndex};
                ++varIndex;
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
            throw std::runtime_error("Header page freeSpaceStart too large for uint16_t");
        }

        return static_cast<std::uint16_t>(freeSpaceStart);
    }

    Page makeEmptyDataPage(uint32_t pageId)
    {
        DataPage dataPage{
            .slots = {},
            .rows = {}};

        PageHeader pageHeader{
            .pageId = pageId,
            .pageType = PageType::DataPage,
            .slotCount = 0,
            .freeSpaceStart = static_cast<uint16_t>(PageHeaderLayout::Size),
            .freeSpaceEnd = static_cast<uint16_t>(PAGE_SIZE),
            .nextPageId = 0};

        return Page{
            .header = pageHeader,
            .data = dataPage};
    }

    Page makeHeaderPage(const std::string &name, const std::string &magic, const std::vector<Column> &columns)
    {
        PageHeader pageHeader{
            .pageId = 0,
            .pageType = PageType::HeaderPage,
            .slotCount = uint16_t{0},
            .freeSpaceStart = uint16_t{0},
            .freeSpaceEnd = static_cast<uint16_t>(PAGE_SIZE),
            .nextPageId = uint32_t{0}};

        HeaderPage tableHeader{
            .magic = magic,
            .version = uint16_t{1},
            .pageSize = PAGE_SIZE,
            .tableName = name,
            .columns = buildStoredColumns(columns),
            .totalRowCount = uint64_t{0},
            .firstDataPageId = uint32_t{0},
            .lastDataPageId = uint32_t{0},
            .nextUnusedPageId = uint32_t{1}};

        pageHeader.freeSpaceStart = calculateHeaderFreeSpaceStart(pageHeader, tableHeader);

        return Page{.header = pageHeader, .data = tableHeader};
    }
}

class BufferManager
{
public:
    explicit BufferManager(std::filesystem::path path)
        : path(std::move(path))
    {
    }
    template <typename Reader>
    Page &getPage(uint32_t pageId, Reader reader)
    {
        auto it = pages.find(pageId);

        if (it != pages.end())
        {
            return it->second.page;
        }

        RawPage rawPage = ::readPageFromFile(path, pageId);
        Page decodedPage = reader(rawPage);

        auto [insertedIt, inserted] = pages.emplace(
            pageId,
            PageFrame{
                .pageId = pageId,
                .page = decodedPage,
                .dirty = false});

        return insertedIt->second.page;
    }

    void markDirty(uint32_t pageId)
    {
        pages.at(pageId).dirty = true;
    }

    void insertAllRows(const std::vector<Row> &rows, Page &hPage)
    {
        if (hPage.header.pageType != PageType::HeaderPage)
        {
            throw std::runtime_error("No header page was passed");
        }
        HeaderPage &headerPage = std::get<HeaderPage>(hPage.data);

        if (rows.empty())
            return;
        if (headerPage.lastDataPageId == 0)
        {
            createDataPage(hPage);
        }
        uint32_t curPageId = headerPage.firstDataPageId;
        size_t rowIndex = 0;
        while (rowIndex < rows.size())
        {
            const Row &row = rows[rowIndex];
            RowValidationResult validation = validateRowAgainstSchema(headerPage.columns, row);
            if (!validation.valid)
            {
                throw std::runtime_error(validation.message);
            }

            Page &dPage = getPage(curPageId, [&headerPage](const RawPage &rawPage)
                                  { return decodeDataPage(rawPage, headerPage); });
            if (enoughSpaceForInsertCheck(dPage, encodedRowSize(headerPage, row)))
            {
                appendRowToExistingDataPage(hPage, dPage, row);
                rowIndex++;
            }
            else if (dPage.header.nextPageId != 0)
            {
                curPageId = dPage.header.nextPageId;
            }
            else
            {
                curPageId = createDataPage(hPage);
            }
        }
    }

    void flushPage(uint32_t pageId)
    {
        PageFrame &frame = pages.at(pageId);

        if (!frame.dirty)
        {
            return;
        }

        RawPage encodedPage = encodeCachedPage(frame.page);

        writePageToFile(pageId, encodedPage);

        frame.dirty = false;
    }

    void flushAll()
    {
        for (auto &[pageId, frame] : pages)
        {
            if (frame.dirty)
            {
                RawPage encodedPage;
                encodedPage = encodeCachedPage(frame.page);
                writePageToFile(pageId, encodedPage);
                frame.dirty = false;
            }
        }
    };
    void setPage(const Page &page, uint32_t pageId)
    {
        pages.insert_or_assign(
            pageId,
            PageFrame{
                .pageId = pageId,
                .page = page,
                .dirty = true});
    };
    void createHeaderPage(
        Page &hPage)
    {
        if (hPage.header.pageType != PageType::HeaderPage)
        {
            throw std::runtime_error("Incorrect page type passed");
        }
        HeaderPage &headerPage = std::get<HeaderPage>(hPage.data);
        PageHeader &headerPageHeader = hPage.header;

        headerPage.firstDataPageId = 0;
        headerPage.lastDataPageId = 0;
        headerPage.nextUnusedPageId = 1;

        Page updatedHeaderPage = Page{.header = headerPageHeader, .data = headerPage};
        setPage(updatedHeaderPage, headerPageHeader.pageId);

        return;
    }

private:
    std::filesystem::path path;
    std::unordered_map<uint32_t, PageFrame> pages;

    RawPage encodeCachedPage(const Page &page)
    {
        if (page.header.pageType == PageType::HeaderPage)
        {
            return encodePage(page);
        }

        if (page.header.pageType == PageType::DataPage)
        {
            auto headerIt = pages.find(0);
            if (headerIt == pages.end())
            {
                throw std::runtime_error("Cannot encode data page without cached header page");
            }

            const HeaderPage &headerPage =
                std::get<HeaderPage>(headerIt->second.page.data);

            const DataPage &dataPage = std::get<DataPage>(page.data);

            std::vector<Row> rows;
            rows.reserve(dataPage.rows.size());

            for (const RowEntry &entry : dataPage.rows)
            {
                rows.push_back(entry.row);
            }

            return encodeDataPage(page.header, headerPage, rows);
        }

        throw std::runtime_error("Unsupported page type while encoding cached page");
    }

    void writePageToFile(
        uint32_t pageId,
        const RawPage &pageData)
    {
        std::fstream file{openOrCreateFile(path)};
        if (!file)
        {
            std::cout << "Failed to open file\n";
            return;
        }

        file.seekp(static_cast<std::streamoff>(pageId) * PAGE_SIZE);

        file.write(
            reinterpret_cast<const char *>(pageData.data()),
            static_cast<std::streamsize>(pageData.size()));
    };

    void appendRowToExistingDataPage(
        Page &hPage,
        Page &dPage,
        const Row &rowToInsert)
    {
        HeaderPage &headerPage = std::get<HeaderPage>(hPage.data);
        PageHeader &headerPageHeader = hPage.header;

        DataPage &dataPage = std::get<DataPage>(dPage.data);
        PageHeader &dataPageHeader = dPage.header;

        if (dataPageHeader.pageType != PageType::DataPage)
        {
            throw std::runtime_error("Page is not a data page");
        }

        const std::size_t encodedSize = encodedRowSize(headerPage, rowToInsert);

        if (encodedSize > std::numeric_limits<uint16_t>::max())
        {
            throw std::runtime_error("Row is too large for uint16_t slot size");
        }

        const uint16_t rowSize = static_cast<uint16_t>(encodedSize);
        const size_t slotSize = encodedSlotSize();

        dataPageHeader.slotCount =
            static_cast<uint16_t>(dataPage.slots.size());

        bool newSlot = true;
        if (!dataPage.slots.empty())
        {
            for (size_t slotIndex = 0; slotIndex < dataPage.slots.size(); ++slotIndex)
            {
                Slot &slot = dataPage.slots[slotIndex];

                if (slot.has(SlotFlag::Deleted) && slot.size >= rowSize)
                {
                    slot.clear(SlotFlag::Deleted);
                    slot.size = static_cast<uint16_t>(rowSize);

                    dataPage.rows.push_back(RowEntry{
                        .slotIndex = static_cast<uint16_t>(slotIndex),
                        .row = rowToInsert,
                    });

                    newSlot = false;
                    break;
                }
            }
        }
        // else

        if (newSlot)
        {
            if (dataPageHeader.freeSpaceEnd < dataPageHeader.freeSpaceStart)
            {
                throw std::runtime_error("Invalid free space pointers");
            }

            size_t freeSpace =
                dataPageHeader.freeSpaceEnd - dataPageHeader.freeSpaceStart;

            size_t neededSpace =
                slotSize + rowSize;

            if (neededSpace > freeSpace)
            {
                throw std::runtime_error("Row does not fit in selected data page");
            }

            uint16_t rowOffset =
                dataPageHeader.freeSpaceStart;

            Slot slot{
                .offset = rowOffset,
                .size = rowSize,
                .flags = 0};

            dataPage.slots.push_back(slot);
            dataPage.rows.push_back(RowEntry{
                .slotIndex = static_cast<uint16_t>(dataPage.slots.size() - size_t{1}),
                .row = rowToInsert,
            });

            dataPageHeader.freeSpaceStart =
                static_cast<uint16_t>(dataPageHeader.freeSpaceStart + rowSize);
            dataPageHeader.freeSpaceEnd =
                static_cast<uint16_t>(dataPageHeader.freeSpaceEnd - slotSize);
            dataPageHeader.slotCount =
                static_cast<uint16_t>(dataPage.slots.size());
        }
        headerPage.totalRowCount++;

        markDirty(dataPageHeader.pageId);
        markDirty(headerPageHeader.pageId);
    };
    uint32_t createDataPage(
        Page &hPage)
    {
        if (hPage.header.pageType != PageType::HeaderPage)
        {
            throw std::runtime_error("Incorrect page type passed");
        }
        HeaderPage &headerPage = std::get<HeaderPage>(hPage.data);
        PageHeader &headerPageHeader = hPage.header;

        uint32_t previousPageId = headerPage.lastDataPageId;
        uint32_t newPageId = headerPage.nextUnusedPageId;

        if (newPageId == 0)
        {
            newPageId = previousPageId + 1;
        }

        if (previousPageId != 0)
        {
            Page &previousPage = getPage(previousPageId, [&headerPage](const RawPage &rawPage)
                                         { return decodeDataPage(rawPage, headerPage); });
            previousPage.header.nextPageId = newPageId;
            markDirty(previousPageId);
        }
        else
        {
            headerPage.firstDataPageId = newPageId;
        }

        Page newDataPage = makeEmptyDataPage(newPageId);

        headerPage.lastDataPageId = newPageId;
        headerPage.nextUnusedPageId = newPageId + 1;
        markDirty(headerPageHeader.pageId);

        setPage(newDataPage, newPageId);

        return newPageId;
    };
    bool enoughSpaceForInsertCheck(const Page &page, size_t rowSize)
    {
        if (page.header.pageType != PageType::DataPage)
        {
            throw std::runtime_error("Invalid page type for insert space check");
        }

        if (page.header.freeSpaceEnd < page.header.freeSpaceStart)
        {
            throw std::runtime_error("Invalid free space pointers");
        }

        const size_t slotSize = encodedSlotSize();

        // Case 1: append new slot + row into normal free space.
        size_t freeSpace =
            page.header.freeSpaceEnd - page.header.freeSpaceStart;

        if (freeSpace >= rowSize + slotSize)
        {
            return true;
        }

        // Case 2: reuse deleted slot.
        // No new slot needed, so only rowSize has to fit.
        const DataPage &dataPage = std::get<DataPage>(page.data);

        for (const Slot &slot : dataPage.slots)
        {
            if (slot.has(SlotFlag::Deleted) && slot.size >= rowSize)
            {
                return true;
            }
        }

        return false;
    }
};

class Table
{
public:
    static Table create(
        std::filesystem::path tablePath,
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns)
    {
        Table table{std::move(tablePath)};

        table.initializeNewTable(tableName, magic, columns);

        return table;
    }

    static Table open(std::filesystem::path tablePath)
    {
        Table table{std::move(tablePath)};

        table.validateHeaderPage();

        return table;
    }

    void insertRows(const BoundInsert &insert)
    {
        Page &hPage = bufferManager.getPage(0, decodeHeaderPage);

        bufferManager.insertAllRows({insert.row}, hPage);

        bufferManager.flushAll();
    }

    std::vector<Row> selectAllRows()
    {
        Page &hPage = bufferManager.getPage(0, decodeHeaderPage);

        return selectAllRowsFromPages(hPage);
    }

    std::vector<Row> selectRows(const BoundSelect &select)
    {
        Page &hPage = bufferManager.getPage(0, decodeHeaderPage);
        std::vector<Row> rows = selectAllRowsFromPages(hPage);

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

private:
    explicit Table(std::filesystem::path tablePath)
        : bufferManager(std::move(tablePath))
    {
    }

    BufferManager bufferManager;

    void initializeNewTable(
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns)
    {
        Page headerPage = makeHeaderPage(tableName, magic, columns);
        Page firstDataPage = makeEmptyDataPage(uint32_t{1});

        HeaderPage &header = std::get<HeaderPage>(headerPage.data);

        header.firstDataPageId = 1;
        header.lastDataPageId = 1;
        header.nextUnusedPageId = 2;
        header.totalRowCount = 0;

        bufferManager.setPage(headerPage, uint32_t{0});

        bufferManager.setPage(firstDataPage, header.firstDataPageId);

        bufferManager.flushAll();
    }

    void validateHeaderPage()
    {
        Page &hPage = bufferManager.getPage(0, decodeHeaderPage);

        if (hPage.header.pageType != PageType::HeaderPage)
        {
            throw std::runtime_error("Page 0 is not a table header page");
        }
    }

    std::vector<Row> selectAllRowsFromPages(Page &hPage)
    {
        std::vector<Row> result;

        HeaderPage &headerPage = std::get<HeaderPage>(hPage.data);

        uint32_t pageId = headerPage.firstDataPageId;

        while (pageId != 0)
        {
            Page &dPage = bufferManager.getPage(pageId, [&headerPage](const RawPage &rawPage)
                                                { return decodeDataPage(rawPage, headerPage); });
            DataPage &dataPage = std::get<DataPage>(dPage.data);

            for (const RowEntry &entry : dataPage.rows)
            {
                result.push_back(entry.row);
            }

            pageId = dPage.header.nextPageId;
        }

        return result;
    }
};

class StorageEngine
{
public:
    explicit StorageEngine(std::filesystem::path dbPath)
        : dbPath(std::move(dbPath)),
          tablesPath(this->dbPath / "tables")
    {
        std::filesystem::create_directories(tablesPath);
    }

    void createTable(
        const std::string &tableName,
        const std::string &magic,
        const std::vector<Column> &columns)
    {
        std::filesystem::path tablePath = getTablePath(tableName);

        if (std::filesystem::exists(tablePath))
        {
            throw std::runtime_error("Table already exists: " + tableName);
        }

        Table::create(tablePath, tableName, magic, columns);
    }

    Table openTable(const std::string &tableName)
    {
        std::filesystem::path tablePath = getTablePath(tableName);

        if (!std::filesystem::exists(tablePath))
        {
            throw std::runtime_error("Table does not exist: " + tableName);
        }

        return Table::open(tablePath);
    }

    const std::filesystem::path &getTablesPath() const
    {
        return tablesPath;
    }

private:
    std::filesystem::path dbPath;
    std::filesystem::path tablesPath;

    std::filesystem::path getTablePath(const std::string &tableName) const
    {
        return tablesPath / (tableName + ".table");
    }
};

class Database
{
public:
    explicit Database(std::filesystem::path dbPath, std::string name)
        : storageEngine(std::move(dbPath))
    {
        dbName = name;
    }

    void createTable(
        const std::string &tableName,
        const std::vector<Column> &columns)
    {
        storageEngine.createTable(tableName, dbName, columns);
    }

    void insertRows(
        const std::string &tableName,
        const std::vector<Row> &rows)
    {
        Table table = storageEngine.openTable(tableName);

        for (const Row &row : rows)
        {
            table.insertRows(BoundInsert{
                .tableName = tableName,
                .row = row});
        }
    }

    std::vector<Row> selectAllRows(const std::string &tableName)
    {
        Table table = storageEngine.openTable(tableName);
        return table.selectAllRows();
    }

    QueryResult execute(const BoundQuery &query)
    {
        return std::visit(
            [this](const auto &boundQuery) -> QueryResult
            {
                using T = std::decay_t<decltype(boundQuery)>;

                if constexpr (std::is_same_v<T, BoundInsert>)
                {
                    return executeInsert(boundQuery);
                }
                else if constexpr (std::is_same_v<T, BoundSelect>)
                {
                    return executeSelect(boundQuery);
                }
            },
            query);
    }

    QueryResult executeSql(const std::string &sql)
    {
        Lexer lexer(sql);
        Parser parser(lexer.tokenize());
        std::unique_ptr<Statement> statement = parser.parseStatement();

        FileCatalog catalog{storageEngine.getTablesPath()};
        QueryValidator validator{catalog};
        BoundQuery query = validator.validate(*statement);

        return execute(query);
    }

    void executeAndPrint(const std::string &sql)
    {
        std::cout << "SQL> " << sql << "\n";

        QueryResult result = executeSql(sql);
        printQueryResult(result);

        std::cout << "\n";
    }

    void printQueryResult(const QueryResult &result) const
    {
        if (result.returnsRows)
        {
            printRows(result.rows);
            std::cout << result.rows.size() << " row(s) selected\n";
            return;
        }

        std::cout << result.affectedRows << " row(s) affected\n";
    }

    QueryResult executeInsert(const BoundInsert &insert)
    {
        Table table = storageEngine.openTable(insert.tableName);
        table.insertRows(insert);

        return QueryResult{
            .rows = {},
            .affectedRows = 1,
            .returnsRows = false};
    }

    QueryResult executeSelect(const BoundSelect &select)
    {
        Table table = storageEngine.openTable(select.tableName);

        return QueryResult{
            .rows = table.selectRows(select),
            .affectedRows = 0,
            .returnsRows = true};
    }

private:
    std::string dbName;

    StorageEngine storageEngine;
};

int main()
{
    std::filesystem::path dbPath{"/tmp/query_test_db"};
    std::filesystem::remove_all(dbPath);

    Database db{dbPath, std::string("query_test_db")};
    std::vector<Column> columns{Column{"a", DataType::Int, false}, Column{"b", DataType::Text, false}, Column{"c", DataType::Text, true}};
    db.createTable("test", columns);

    std::vector<std::string> testQueries{
        "INSERT INTO test (a, b, c) VALUES (1, 'Appel', 'Peer');",
        "INSERT INTO test (a, b, c) VALUES (2, 'Mandarijn', 'Banaan');",
        "INSERT INTO test (a, b, c) VALUES (3, 'Kiwi', NULL);",
        "SELECT * FROM test;",
        "SELECT a, b FROM test WHERE a >= 2;",
        "SELECT b, c FROM test WHERE c IS NULL;",
        "SELECT test.* FROM test WHERE b >= 'Mandarijn';",
        "SELECT a, c FROM test WHERE a > 1 AND c IS NOT NULL;"};

    for (const std::string &query : testQueries)
    {
        db.executeAndPrint(query);
    }
}
// { int32_t{1}, std::string("Alice"), std::monostate{} }

// 14 00 00 00      row_size = 20 bytes

// 03 00 00 00      value_count = 3

// 01               value type = INT
// 01 00 00 00      int value = 1

// 02               value type = TEXT
// 05 00 00 00      string length = 5
// 41 6C 69 63 65   string bytes = Alice

// 00               value type = NULL
