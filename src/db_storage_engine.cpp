#include "db_storage_engine.h"
#include "db_query_exception.h"
#include "db_query_validator.h"
#include "db_linux_file.h"
#include "db_write.h"
#include "db_table.h"
#include "db_index.h"
#include "db_constraint_validator.h"
#include "db_expression_evaluator.h"
#include "db_page_factory.h"

#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <ranges>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <zlib.h>
#include <vector>
#include <array>
#include <cstddef>
#include <cstring>

namespace
{

    std::string uuidToString(const uuid_t &id)
    {
        char buffer[37]; // 36 characters + null terminator
        uuid_unparse_lower(id, buffer);
        std::string result{buffer};
        std::erase(result, '-');
        return result;
    }
    std::uint32_t checksum(std::span<const std::byte> bytes)
    {
        return static_cast<std::uint32_t>(
            crc32_z(0,
                    reinterpret_cast<const Bytef *>(bytes.data()),
                    bytes.size()));
    }
    // hardcoded for now
    constexpr std::array<char, 8> MetaMagic{
        'D', 'B', 'M', 'E', 'T', 'A', '\0', '\0'};
    constexpr std::size_t META_FILE_SIZE = 8 + 16 + 8 + 4; // magic, uuid, version, epoch, generation, checksum

    auto extractTableNames = [](const std::vector<std::string> &names)
    {
        std::vector<std::string> result;

        for (const auto &name : names)
        {
            if (name.ends_with(".table"))
            {
                result.push_back(name.substr(0, name.size() - 6));
            }
        }

        return result;
    };

    std::vector<LocatedRow> getLocatedRowsFromWhereExpr(
        const std::unique_ptr<BoundExpr> &where, Table &table)
    {
        auto cursor = table.scan();
        std::vector<LocatedRow> result;
        while (auto row = cursor.nextLocated())
        {
            if (!where || evaluatePredicate(*where, row->row))
                result.push_back(std::move(*row));
        }
        return result;
    }

    auto buildRowsToUpdate(const BoundUpdate &update, const std::vector<LocatedRow> &originalRows)
    {
        std::vector<LocatedRow> updatedRows{};
        updatedRows.reserve(originalRows.size());
        for (const auto &located : originalRows)
        {
            const Row &original = located.row;
            Row updated = original;

            for (const auto &assignment : update.assignments)
            {
                auto value = evaluateValue(*assignment.expr, original);

                // Convert value to the target column's type here.
                updated.values.at(assignment.columnId) = std::move(value);
            }
            updatedRows.push_back({located.id, std::move(updated)});
        }
        return updatedRows;
    }

}

StorageEngine::StorageEngine(
    const std::filesystem::path &path, LinuxDirectory lockedRoot, const uuid_t &dbId)
    : dbPath_(path),
      dbRootDirectory_(std::move(lockedRoot)),
      tablesDirectory_(dbPath_ / "tables"),
      indexDirectory_(dbPath_ / "indexes"),
      journalDirectory_(dbPath_ / "journal"),
      statementRecovery_(dbRootDirectory_, dbId)
{
}

void StorageEngine::createMetaFile(const std::filesystem::path &dbPath, const uuid_t &dbId)
{
    LinuxDirectory root{dbPath};
    LinuxFile dbMetaFile{root, "db.meta", LinuxFile::OpenMode::CreateNew};
    ByteWriter writer{META_FILE_SIZE};

    writer.writeBytes(MetaMagic.data(), MetaMagic.size());

    writer.writeBytes(dbId, 16);

    writer.writeUnsigned<std::uint64_t>(static_cast<std::uint64_t>(1)); // version
    // writer.writeUnsigned<std::uint64_t>(static_cast<std::uint64_t>(0)); // epoch
    // writer.writeUnsigned<std::uint64_t>(static_cast<std::uint64_t>(0)); // generation
    const auto content = std::span<const std::byte>{writer.bytes()}
                             .first(writer.position());
    writer.writeUnsigned<std::uint32_t>(checksum(content)); // checksum
    dbMetaFile.writeAllAt(0, writer.bytes());
    dbMetaFile.sync();
}

StorageEngine StorageEngine::create(
    const std::filesystem::path &path)
{
    // Create the database root before creating its subdirectories.

    uuid_t dbId;
    uuid_generate_random(dbId);
    if (::mkdir(path.c_str(), 0700) == -1)
    {
        const int error = errno;

        throwError("mkdir", error, path);
    }
    LinuxDirectory root{path};
    root.lockExclusive();

    if (::mkdir((path / "tables").c_str(), 0700) == -1)
    {
        const int error = errno;

        throwError("mkdir", error, path / "tables");
    }

    if (::mkdir((path / "journal").c_str(), 0700) == -1)
    {
        const int error = errno;

        throwError("mkdir", error, path / "journal");
    }

    if (::mkdir((path / "indexes").c_str(), 0700) == -1)
    {
        const int error = errno;

        throwError("mkdir", error, path / "indexes");
    }

    createMetaFile(path, dbId);

    root.sync();
    LinuxDirectory rootParent{
        std::filesystem::canonical(path).parent_path()};

    rootParent.sync();

    return StorageEngine{path, std::move(root), dbId};
}

StorageEngine StorageEngine::open(
    const std::filesystem::path &path)
{
    LinuxDirectory root{path};
    root.lockExclusive();

    LinuxFile dbMetaFile{root, "db.meta", LinuxFile::OpenMode::OpenExisting};

    std::array<std::byte, META_FILE_SIZE> metaBytes{};
    dbMetaFile.readExactAt(0, metaBytes);
    ByteReader reader{metaBytes};

    std::array<char, 8> metaMagic;
    reader.readBytes(metaMagic.data(), metaMagic.size());
    if (metaMagic != MetaMagic)
    {
        throw std::runtime_error("Invalid database meta file");
    }

    uuid_t dbId;
    reader.readBytes(dbId, sizeof(dbId)); // dbId

    const auto version = reader.readUnsigned<std::uint64_t>(); // version
    if (version != 1)
    {
        throw std::runtime_error("Unsupported database version");
    }
    // reader.readUnsigned<std::uint64_t>(); // epoch
    // reader.readUnsigned<std::uint64_t>(); // generation
    const auto checksumValue = reader.readUnsigned<std::uint32_t>();

    const auto content = std::span<const std::byte>{metaBytes}.first(META_FILE_SIZE - sizeof(checksumValue));
    if (checksum(content) != checksumValue)
    {
        throw std::runtime_error("Invalid database meta file checksum");
    }

    StorageEngine engine{path, std::move(root), dbId};

    // Staging journals never authorize database writes. A crash during header
    // construction or before its rename may leave this disposable file behind.
    if (engine.journalDirectory_.entryExists("journal.pending"))
    {
        engine.journalDirectory_.deleteFile("journal.pending");
        engine.journalDirectory_.sync();
    }

    if (engine.journalDirectory_.entryExists("journal.log"))
    {
        engine.statementRecovery_.initializeForRecover();
        engine.statementRecovery_.rollback();
    }

    return engine;
}

StorageEngine::TableDdlPlan StorageEngine::planTableDdl(
    const std::vector<std::string> &requested, bool cascade, TableOperation operation)
{
    requireReadable();
    TableDdlPlan plan{{requested.begin(), requested.end()}, {}};
    if (plan.targets.empty())
        throw QueryException(QueryErrorCode::Validation, "Expected at least one table");
    for (const auto &name : plan.targets)
        if (!tableExists(name))
            throw QueryException(QueryErrorCode::Validation, "Table does not exist: " + name);

    std::vector<HeaderPage> schemas;
    for (const auto &name : extractTableNames(tablesDirectory_.listFileNames()))
        schemas.push_back(getTableHeader(name));

    // TRUNCATE needs the transitive closure; DROP only removes incoming FKs
    // from surviving tables. A set handles duplicate targets and FK cycles.
    bool added;
    do
    {
        added = false;
        for (const auto &schema : schemas)
        {
            if (plan.targets.contains(schema.tableName))
                continue;
            for (const auto &constraint : schema.constraints)
            {
                const auto *fk = std::get_if<BoundForeignKeyConstraintExpr>(&constraint);
                if (!fk || !plan.targets.contains(fk->referencedTableName))
                    continue;
                if (!cascade)
                    throw QueryException(QueryErrorCode::Constraint, std::format(
                                                                         "FOREIGN KEY constraint '{}' in table '{}' references table '{}'; {} requires CASCADE",
                                                                         fk->constraintName, schema.tableName, fk->referencedTableName,
                                                                         operation == TableOperation::Drop ? "DROP TABLE" : "TRUNCATE TABLE"));
                if (operation == TableOperation::Truncate)
                    added = plan.targets.insert(schema.tableName).second || added;
                else
                    plan.referencingTables.insert(schema.tableName);
                break;
            }
        }
    } while (added);
    return plan;
}

QueryResult StorageEngine::dropTable(const BoundDropTable &drop)
{
    const auto plan = planTableDdl(drop.tableNames, drop.cascade, TableOperation::Drop);
    std::vector<std::string> indexNames;
    for (const auto &name : plan.targets)
    {
        auto names = indexNamesForTable(name);
        indexNames.insert(indexNames.end(), names.begin(), names.end());
    }
    for (const auto &name : plan.referencingTables)
        openTable(name).removeForeignKeysTo(plan.targets);
    for (const auto &name : indexNames)
        statementRecovery_.deleteFile(std::filesystem::path{"indexes"} / (name + ".index"));
    for (const auto &name : plan.targets)
        statementRecovery_.deleteFile(name + ".table");
    return QueryResult{.columns = {}, .rows = {}, .affectedRows = 0, .returnsRows = false};
}

QueryResult StorageEngine::dropIndex(const BoundDropIndex &drop)
{
    if (!indexExists(drop.indexName))
        throw QueryException(QueryErrorCode::Validation, "Index does not exist: " + drop.indexName);
    statementRecovery_.deleteFile(std::filesystem::path{"indexes"} / (drop.indexName + ".index"));
    return QueryResult{.columns = {}, .rows = {}, .affectedRows = 0, .returnsRows = false};
}

QueryResult StorageEngine::truncateTable(const BoundTruncateTable &truncate)
{
    const auto plan = planTableDdl(truncate.tableNames, truncate.cascade, TableOperation::Truncate);
    for (const auto &name : plan.targets)
    {
        openTable(name).truncateTable();
        for (auto &index : indexesForTable(name))
            index.truncate();
    }
    return QueryResult{.columns = {}, .rows = {}, .affectedRows = 0, .returnsRows = false};
}

QueryResult StorageEngine::insertRows(const BoundInsert &insert)
{
    const auto schema = getTableHeader(insert.tableName);
    StorageConstraintLookup lookup{*this};
    ConstraintValidator constraintValidator{lookup};

    const auto validationResult = constraintValidator.validateConstraint(schema, insert.rows);
    if (!validationResult.valid)
        throw QueryException(QueryErrorCode::Constraint, validationResult.message.value());
    auto table = openTable(insert.tableName);

    const auto &locatedRows = table.insertRows(insert);

    auto indexes = indexesForTable(insert.tableName);
    if (!indexes.empty())
    {
        for (auto &index : indexes)
        {
            index.insertIndexes(locatedRows);
        }
    }

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = insert.rows.size(),
        .returnsRows = false};
}

QueryResult StorageEngine::updateRows(const BoundUpdate &update)
{
    const auto schema = getTableHeader(update.tableName);
    StorageConstraintLookup lookup{*this};
    ConstraintValidator constraintValidator{lookup};
    auto table = openTable(update.tableName);

    const auto rowsToUpdate = getLocatedRowsFromWhereExpr(update.where, table);
    const auto updatedLocatedRows = buildRowsToUpdate(update, rowsToUpdate);
    std::set<RowId> excludedRows;
    for (const auto &row : rowsToUpdate)
        excludedRows.insert(row.id);
    const auto tableNames = extractTableNames(tablesDirectory_.listFileNames());

    auto updatedRows = updatedLocatedRows | std::views::transform(
                                                [](const LocatedRow &located) -> const Row &
                                                {
                                                    return located.row;
                                                });
    const auto validationResult = constraintValidator.validateUpdateConstraint(
        schema, updatedRows, excludedRows, tableNames, *this);
    if (!validationResult.valid)
        throw QueryException(QueryErrorCode::Constraint, validationResult.message.value());
    auto indexes = indexesForTable(update.tableName);
    // Remove the entire old key set first so rows in this UPDATE can exchange
    // unique keys. All table/index writes share the statement's undo journal.
    for (auto &index : indexes)
        index.deleteIndexes(rowsToUpdate);

    const auto storedRows = table.updateRows(updatedLocatedRows);
    for (auto &index : indexes)
        index.insertIndexes(storedRows);

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = updatedRows.size(),
        .returnsRows = false};
}

QueryResult StorageEngine::deleteRows(const BoundDelete &del)
{
    if (!del.from)
        throw std::runtime_error("DELETE requires a FROM clause");
    const auto *target = std::get_if<BoundNamedTableRef>(&del.from->value);
    if (!target)
        throw std::runtime_error("Only single-table DELETE is supported");
    const auto &tableName = target->tableName;
    StorageConstraintLookup lookup{*this};
    ConstraintValidator constraintValidator{lookup};

    const auto &fileNames = tablesDirectory_.listFileNames();
    auto tableNames = extractTableNames(fileNames);
    auto table = openTable(tableName);

    const auto &delRows = getLocatedRowsFromWhereExpr(del.where, table);

    const auto validationResult = constraintValidator.validateFkConstraintForDelete(tableName, tableNames, *this, delRows);
    if (!validationResult.valid)
        throw QueryException(QueryErrorCode::Constraint, validationResult.message.value());

    const auto deletedCount = table.deleteRows(delRows);

    auto indexes = indexesForTable(tableName);
    if (!indexes.empty())
    {
        for (auto &index : indexes)
        {
            index.deleteIndexes(delRows);
        }
    }

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = deletedCount,
        .returnsRows = false};
}

Index StorageEngine::createIndex(const BoundCreateIndex &createIndex, const IndexBulkLoadOptions &options)
{
    requireReadable();
    if (indexDirectory_.entryExists(createIndex.name + ".index"))
    {
        throw QueryException(QueryErrorCode::Validation, "Index already exists: " + createIndex.name);
    }

    Table table = openTable(createIndex.tableName);
    const auto &tableSchema = getTableHeader(createIndex.tableName);

    std::vector<Column> selectedColumns{};
    selectedColumns.reserve(createIndex.columnIds.size());

    for (ColumnId id : createIndex.columnIds)
    {
        selectedColumns.push_back(tableSchema.columns.at(id));
    }

    // Reassigns columnIndex and rebuilds storage offsets.

    CreateIndexPayload createIndexPayload{.indexName = createIndex.name, .tableName = createIndex.tableName, .magic = "MYIX", .keyColumns = createIndex.columnIds, .columns = buildStoredColumns(selectedColumns), .unique = createIndex.unique};
    auto index = Index::create(indexDirectory_, createIndexPayload, statementRecovery_);
    index.bulkLoad(table, options);
    return index;
}

bool StorageEngine::tableExists(const std::string &name) const
{
    requireReadable();
    return tablesDirectory_.entryExists(name + ".table");
}

bool StorageEngine::indexExists(const std::string &name) const
{
    requireReadable();
    return indexDirectory_.entryExists(name + ".index");
}

HeaderPage StorageEngine::getTableHeader(const std::string &tableName) const
{
    requireReadable();

    const auto fileName = tableName + ".table";

    if (!tableExists(tableName))
    {
        throw QueryException(QueryErrorCode::Validation, "Table does not exist: " + tableName);
    }

    RawPage rawPage = readPageFromFile(tablesDirectory_, fileName, HeaderPageId);
    Page page = decodeHeaderPage(rawPage);

    return std::get<HeaderPage>(page.data);
}

std::vector<Column> StorageEngine::getTableSchema(const std::string &tableName) const
{
    requireReadable();

    const auto fileName = tableName + ".table";

    if (!tableExists(tableName))
    {
        throw QueryException(QueryErrorCode::Validation, "Table does not exist: " + tableName);
    }

    RawPage rawPage = readPageFromFile(tablesDirectory_, fileName, HeaderPageId);
    Page page = decodeHeaderPage(rawPage);

    return std::get<HeaderPage>(page.data).columns;
}

void StorageEngine::createIndexForPkAndUniqueConstraints(const BoundCreateTable &createTable)
{
    for (const auto &constraint : createTable.constraints)
    {
        const auto *pk =
            std::get_if<BoundPrimaryKeyConstraintExpr>(&constraint);
        const auto *uc =
            std::get_if<BoundUniqueConstraintExpr>(&constraint);

        if (pk)
        {
            uuid_t pkId;
            uuid_generate_random(pkId);

            const auto indexName = std::format("pk_{}", uuidToString(pkId));
            std::vector<Column>
                selectedColumns{};
            std::vector<ColumnId> selectedColumnIds = pk->columnIds;
            for (const auto colId : selectedColumnIds)
            {
                selectedColumns.push_back(createTable.columns.at(colId));
            }

            // Reassigns columnIndex and rebuilds storage offsets.

            CreateIndexPayload createIndexPayload{.indexName = indexName, .tableName = createTable.tableName, .magic = "MYIX", .keyColumns = selectedColumnIds, .columns = buildStoredColumns(selectedColumns), .unique = true};
            Index::create(indexDirectory_, createIndexPayload, statementRecovery_);
            continue;
        }

        if (uc)
        {
            uuid_t ucId;
            uuid_generate_random(ucId);

            const auto indexName = std::format("unique_{}", uuidToString(ucId));
            std::vector<Column>
                selectedColumns{};
            std::vector<ColumnId> selectedColumnIds = uc->columnIds;
            for (const auto colId : selectedColumnIds)
            {
                selectedColumns.push_back(createTable.columns.at(colId));
            }

            // Reassigns columnIndex and rebuilds storage offsets.

            CreateIndexPayload createIndexPayload{.indexName = indexName, .tableName = createTable.tableName, .magic = "MYIX", .keyColumns = selectedColumnIds, .columns = buildStoredColumns(selectedColumns), .unique = true};
            Index::create(indexDirectory_, createIndexPayload, statementRecovery_);
            continue;
        }
    }
}

Table StorageEngine::createTable(
    const BoundCreateTable &createTable)
{
    requireReadable();
    if (tablesDirectory_.entryExists(createTable.tableName + ".table"))
    {
        throw QueryException(QueryErrorCode::Validation, "Table already exists: " + createTable.tableName);
    }

    StorageConstraintLookup lookup{*this};
    ConstraintValidator constraintValidator{lookup};

    const auto validationResult = constraintValidator.validateFkConstraintForTableCreation(createTable.constraints, *this);

    if (!validationResult.valid)
        throw QueryException(QueryErrorCode::Constraint, validationResult.message.value());

    createIndexForPkAndUniqueConstraints(createTable);

    return Table::create(
        tablesDirectory_,
        CreateTablePayload{.tableName = createTable.tableName, .magic = "MYDB", .columns = createTable.columns, .constraints = createTable.constraints},
        statementRecovery_);
}

Table StorageEngine::openTable(const std::string &tableName)
{
    requireReadable();

    const auto fileName = getTablePath(tableName).filename();
    if (!tablesDirectory_.entryExists(fileName))
    {
        throw QueryException(QueryErrorCode::Validation, "Table does not exist: " + tableName);
    }

    return Table::open(tablesDirectory_, fileName, statementRecovery_);
}

Index StorageEngine::openIndex(const std::string &indexName)
{
    requireReadable();

    const auto fileName = getIndexPath(indexName).filename();
    if (!indexDirectory_.entryExists(fileName))
    {
        throw std::runtime_error("Index does not exist: " + indexName);
    }

    return Index::open(indexDirectory_, fileName, statementRecovery_);
}

Index StorageEngine::openIndex(const std::filesystem::path &fileName)
{
    requireReadable();

    if (!indexDirectory_.entryExists(fileName))
    {
        throw std::runtime_error("Index does not exist: " + fileName.string());
    }

    return Index::open(indexDirectory_, fileName, statementRecovery_);
}
std::vector<Index> StorageEngine::indexesForTable(const std::string &tableName)
{
    const auto &fileNames = indexDirectory_.listFileNames();
    std::vector<Index> result{};
    result.reserve(fileNames.size());
    for (const auto &fileName : fileNames)
    {
        auto index = openIndex(std::filesystem::path{fileName});
        const auto &name = index.getTableName();
        if (name == tableName)
        {
            result.push_back(std::move(index));
        }
    }
    return result;
}

std::vector<std::string> StorageEngine::indexNamesForTable(const std::string &tableName)
{
    const auto &fileNames = indexDirectory_.listFileNames();

    std::vector<std::string> result;
    result.reserve(fileNames.size());

    for (const auto &fileName : fileNames)
    {
        auto index = openIndex(std::filesystem::path{fileName});

        if (index.getTableName() == tableName)
        {
            result.push_back(index.getIndexName());
        }
    }
    return result;
}
