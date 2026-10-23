#include "db_storage_engine.h"
#include "db_linux_file.h"
#include "db_write.h"
#include "db_constraint_validator.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
    for (auto row : insert.rows)
    {
        const auto validationResult = constraintValidator.validateConstraint(schema, row);
        if (!validationResult.valid)
            throw std::runtime_error(validationResult.message);
    }
    auto table = openTable(insert.tableName);

    table.insertRows(insert);

    return QueryResult{
        .columns = {},
        .rows = {},
        .affectedRows = insert.rows.size(),
        .returnsRows = false};
}

bool StorageEngine::tableExists(const std::string &name) const
{
    requireReadable();
    return std::filesystem::exists(getTablePath(name));
}

HeaderPage StorageEngine::getTableHeader(const std::string &tableName) const
{
    requireReadable();

    const auto fileName = getTablePath(tableName).filename();
    if (!tablesDirectory_.entryExists(fileName))
    {
        throw std::runtime_error("Table does not exist: " + tableName);
    }

    RawPage rawPage = readPageFromFile(tablesDirectory_, fileName, 0);
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

    if (tablesDirectory_.entryExists(getTablePath(tableName).filename()))
    {
        throw std::runtime_error("Table already exists: " + tableName);
    }

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
