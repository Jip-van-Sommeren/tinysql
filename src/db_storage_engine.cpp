#include "db_storage_engine.h"
#include "db_linux_file.h"
#include "db_write.h"
#include "db_constraint_validator.h"
#include "db_expression_evaluator.h"

#include <filesystem>
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

QueryResult StorageEngine::insertRows(const BoundInsert &insert)
{
    const auto schema = getTableHeader(insert.tableName);
    StorageConstraintLookup lookup{*this};
    ConstraintValidator constraintValidator{lookup};

    const auto validationResult = constraintValidator.validateConstraint(schema, insert.rows);
    if (!validationResult.valid)
        throw std::runtime_error(validationResult.message.value());
    auto table = openTable(insert.tableName);

    table.insertRows(insert);

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
        throw std::runtime_error(validationResult.message.value());
    table.updateRows(updatedLocatedRows);

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
        throw std::runtime_error(validationResult.message.value());

    const auto deletedCount = table.deleteRows(delRows);

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = deletedCount,
        .returnsRows = false};
}

bool StorageEngine::tableExists(const std::string &name) const
{
    requireReadable();
    return tablesDirectory_.entryExists(name + ".table");
}

HeaderPage StorageEngine::getTableHeader(const std::string &tableName) const
{
    requireReadable();

    const auto fileName = tableName + ".table";

    if (!tableExists(tableName))
    {
        throw std::runtime_error("Table does not exist: " + tableName);
    }

    RawPage rawPage = readPageFromFile(tablesDirectory_, fileName, HeaderPageId);
    Page page = decodeHeaderPage(rawPage);

    return std::get<HeaderPage>(page.data);
}

Table StorageEngine::createTable(
    const std::string &tableName,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<BoundConstraint> &constraints)
{
    requireReadable();
    if (tablesDirectory_.entryExists(tableName + ".table"))
    {
        throw std::runtime_error("Table already exists: " + tableName);
    }

    StorageConstraintLookup lookup{*this};
    ConstraintValidator constraintValidator{lookup};

    const auto validationResult = constraintValidator.validateFkConstraintForTableCreation(constraints, *this);
    if (!validationResult.valid)
        throw std::runtime_error(validationResult.message.value());

    return Table::create(
        tablesDirectory_,

        tableName,
        magic,
        columns,
        constraints,
        statementRecovery_);
}

Table StorageEngine::openTable(const std::string &tableName)
{
    requireReadable();

    const auto fileName = getTablePath(tableName).filename();
    if (!tablesDirectory_.entryExists(fileName))
    {
        throw std::runtime_error("Table does not exist: " + tableName);
    }

    return Table::open(tablesDirectory_, fileName, statementRecovery_);
}
