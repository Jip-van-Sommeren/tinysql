#include "db_journal_file.h"

#include "db_read.h"
#include "db_write.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <zlib.h>
#include <uuid/uuid.h>
namespace
{
    std::string encodedPath(const std::filesystem::path &path)
    {
        const std::string value = path.generic_string();
        if (value.empty() || value.size() > JournalFile::MaxPathBytes ||
            value.find('\0') != std::string::npos || path.has_root_path())
        {
            throw std::runtime_error("Invalid journal relative file path");
        }
        for (const auto &component : path)
        {
            if (component == "..")
            {
                throw std::runtime_error("Journal path must not contain '..'");
            }
        }
        return value;
    }

    std::uint32_t checksum(std::span<const std::byte> bytes)
    {
        return static_cast<std::uint32_t>(
            crc32_z(0,
                    reinterpret_cast<const Bytef *>(bytes.data()),
                    bytes.size()));
    }
    // path_length and  pageId = 2* sizeof(std::uint32_t)
    constexpr std::size_t RecordOverhead = 2 * sizeof(std::uint32_t) + PAGE_SIZE + sizeof(std::uint8_t);
}

std::size_t PageBeforeImage::size() const
{
    return RecordOverhead + encodedPath(relativeFilePath).size();
}

JournalFile::JournalFile(const std::filesystem::path &path, LinuxFile::OpenMode mode)
    : file_(path, mode), appendOffset_(file_.size())
{
}

std::uint64_t JournalFile::findRecoveryEnd(
    std::uint64_t fileSize)
{
    std::uint64_t offset{0};

    while (offset < fileSize)
    {

        if (fileSize - offset < JournalRecordHeaderBytes)
        {
            return offset;
        }
        const auto recordStart = offset;

        // Every record starts with [type:u8][path length:u32].
        JournalRecordHeader header{};
        std::array<std::byte, JournalRecordHeaderBytes> prefixBytes{};
        file_.readExactAt(recordStart, prefixBytes);
        ByteReader prefixReader(prefixBytes);

        const auto journalType = static_cast<JournalRecordType>(
            prefixReader.readUnsigned<std::uint8_t>());
        const auto recordSize = prefixReader.readUnsigned<std::uint64_t>();
        // const auto sequenceNumber = prefixReader.readUnsigned<std::uint32_t>();
        // const auto checksum = prefixReader.readUnsigned<std::uint32_t>();
        if (journalType != JournalRecordType::FileBeforeImage && journalType != JournalRecordType::PageBeforeImage)
        {
            throw std::runtime_error("invalid Journal Record Type");
        }
        if (recordSize > fileSize - recordStart)
        {
            return recordStart;
        }

        offset += recordSize;
    }
    return offset;
}

JournalHeader JournalFile::readJournalHeader()
{
    std::array<std::byte, JournalHeaderBytes> headerBytes{};
    file_.readExactAt(0, headerBytes);
    ByteReader reader(headerBytes);
    JournalHeader header{};

    std::array<char, 8> magic;
    reader.readBytes(magic.data(), magic.size());
    if (magic != JournalMagic)
    {
        throw std::runtime_error("Invalid journal header magic");
    }

    const auto version = reader.readUnsigned<std::uint32_t>();
    const auto headerLength = reader.readUnsigned<std::uint32_t>();
    uuid_t dbId;
    reader.readBytes(dbId, sizeof(dbId));

    uuid_t statementId;
    reader.readBytes(statementId, sizeof(statementId));
    const auto pageSize = reader.readUnsigned<std::uint64_t>();
    const auto FileChecksum = reader.readUnsigned<std::uint32_t>();

    const auto content = std::span<const std::byte>{headerBytes}.first(JournalHeaderBytes - sizeof(FileChecksum));
    if (checksum(content) != FileChecksum)
    {
        throw std::runtime_error("Invalid journal header checksum");
    }

    return JournalHeader{
        .magic = magic,
        .version = version,
        .headerLength = headerLength,
        .dbId = *dbId,
        .statementId = *statementId,
        .pageSize = pageSize,
        .checksum = FileChecksum};
}

std::optional<JournalRecord> JournalFile::readRecord(
    std::uint64_t &offset, std::uint64_t fileSize)
{

    if (offset == fileSize)
    {
        return std::nullopt;
    }
    if (offset > fileSize || fileSize - offset < JournalRecordHeaderBytes)
    {
        throw std::runtime_error("Truncated journal record length");
    }
    const auto recordStart = offset;

    // Every record starts with [type:u8][path length:u32].
    std::array<std::byte, JournalRecordHeaderBytes> headerBytes{};
    file_.readExactAt(recordStart, headerBytes);
    ByteReader prefixReader(headerBytes);

    const auto journalType = static_cast<JournalRecordType>(
        prefixReader.readUnsigned<std::uint8_t>());
    const auto recordSize = prefixReader.readUnsigned<std::uint64_t>();
    const auto sequenceNumber = prefixReader.readUnsigned<std::uint32_t>();
    const auto checksum = prefixReader.readUnsigned<std::uint32_t>();
    JournalRecordHeader header{
        .type = journalType,
        .recordSize = recordSize,
        .sequenceNumber = sequenceNumber,
        .checksum = checksum};

    if (journalType != JournalRecordType::FileBeforeImage && journalType != JournalRecordType::PageBeforeImage)
    {
        throw std::runtime_error("invalid Journal Record Type");
    }

    if (recordSize > fileSize - recordStart)
    {
        throw std::runtime_error("Truncated journal record");
    }

    std::vector<std::byte> bytes(recordSize);
    file_.readExactAt(recordStart + JournalRecordHeaderBytes,
                      std::span<std::byte>{bytes});
    ByteReader reader(bytes);

    if (journalType == JournalRecordType::FileBeforeImage)
    {
        FileBeforeImage record{};
        record.relativeFilePath = reader.readString();
        (void)encodedPath(record.relativeFilePath);
        record.originalSize = reader.readUnsigned<std::uint64_t>();
        const auto newFile = reader.readUnsigned<std::uint8_t>();
        if (newFile > 1)
        {
            throw std::runtime_error("Invalid journal new-file flag");
        }
        record.newFile = newFile != 0;

        offset = recordStart + recordSize;
        return JournalRecord{header, record};
    }
    // Dont need to check for Other JournalRecordType again since invalid type should throw in the section above
    PageBeforeImage record{};
    record.relativeFilePath = reader.readString();
    (void)encodedPath(record.relativeFilePath);
    record.pageId = reader.readUnsigned<std::uint32_t>();
    reader.readBytes(record.originalPage.data(), record.originalPage.size());

    offset = recordStart + recordSize;
    return JournalRecord{header, record};
}

std::optional<JournalRecord> JournalFile::readNext(std::uint64_t end)
{
    return readRecord(readOffset_, end);
}

std::optional<JournalRecord> JournalFile::readNext()
{
    return readRecord(readOffset_, file_.size());
}

std::uint64_t JournalFile::writeFileBeforeImage(const FileBeforeImage &fileBeforeImage)
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }

    const std::string path = encodedPath(fileBeforeImage.relativeFilePath);
    ByteWriter writer(path.size() + 4 + 8 + 1); // path, path length, original size, new file flag
    // writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(JournalRecordType::FileBeforeImage));
    writer.writeString(path);
    writer.writeUnsigned<std::uint64_t>(fileBeforeImage.originalSize);
    writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(fileBeforeImage.newFile));
    if (writer.size() > std::numeric_limits<std::uint64_t>::max() - appendOffset_)
    {
        throw std::out_of_range("Journal append offset overflow");
    }
    try
    {
        file_.writeAllAt(appendOffset_, writer.bytes());
    }
    catch (...)
    {
        // A partial record may have reached disk. Never append past it on this
        // object or report it as a successfully synchronized record stream.
        appendFailed_ = true;
        throw;
    }
    appendOffset_ += writer.size();
    return appendOffset_;
}
std::uint64_t JournalFile::writePage(const PageBeforeImage &pageBeforeImage)
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }

    const std::string path = encodedPath(pageBeforeImage.relativeFilePath);
    ByteWriter writer(path.size() + 4 + 4 + PAGE_SIZE); // path, path length, pageId, original page
    // writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(JournalRecordType::PageBeforeImage));
    writer.writeString(path);
    writer.writeUnsigned<std::uint32_t>(pageBeforeImage.pageId);
    writer.writeBytes(pageBeforeImage.originalPage.data(), pageBeforeImage.originalPage.size());

    if (writer.size() > std::numeric_limits<std::uint64_t>::max() - appendOffset_)
    {
        throw std::out_of_range("Journal append offset overflow");
    }
    try
    {
        file_.writeAllAt(appendOffset_, writer.bytes());
    }
    catch (...)
    {
        // A partial record may have reached disk. Never append past it on this
        // object or report it as a successfully synchronized record stream.
        appendFailed_ = true;
        throw;
    }
    appendOffset_ += writer.size();
    return appendOffset_;
}

std::uint64_t JournalFile::writeJournalHeader(const JournalHeader &header)
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }
    ByteWriter writer(JournalHeaderBytes);

    writer.writeBytes(JournalMagic.data(), JournalMagic.size());

    writer.writeUnsigned<std::uint32_t>(header.version);
    writer.writeUnsigned<std::uint64_t>(header.headerLength);
    writer.writeBytes(header.dbId, 16);
    writer.writeBytes(header.statementId, 16);
    writer.writeUnsigned<std::uint64_t>(header.pageSize);

    const auto content = std::span<const std::byte>{writer.bytes()}
                             .first(writer.position());
    writer.writeUnsigned<std::uint32_t>(checksum(content));

    if (writer.size() > std::numeric_limits<std::uint64_t>::max() - appendOffset_)
    {
        throw std::out_of_range("Journal append offset overflow");
    }
    try
    {
        file_.writeAllAt(appendOffset_, writer.bytes());
    }
    catch (...)
    {
        // A partial record may have reached disk. Never append past it on this
        // object or report it as a successfully synchronized record stream.
        appendFailed_ = true;
        throw;
    }
    appendOffset_ += writer.size();
    return appendOffset_;
}

std::uint64_t JournalFile::writeJournalRecordHeader(const JournalRecordHeader &header)
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }
    ByteWriter writer(JournalRecordHeaderBytes);

    writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(header.type));
    writer.writeUnsigned<std::uint64_t>(header.recordSize);
    writer.writeUnsigned<std::uint32_t>(header.sequenceNumber);
    writer.writeUnsigned<std::uint32_t>(header.checksum);

    if (writer.size() > std::numeric_limits<std::uint64_t>::max() - appendOffset_)
    {
        throw std::out_of_range("Journal append offset overflow");
    }
    try
    {
        file_.writeAllAt(appendOffset_, writer.bytes());
    }
    catch (...)
    {
        // A partial record may have reached disk. Never append past it on this
        // object or report it as a successfully synchronized record stream.
        appendFailed_ = true;
        throw;
    }
    appendOffset_ += writer.size();
    return appendOffset_;
}

void JournalFile::sync()
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }
    file_.sync();
}
