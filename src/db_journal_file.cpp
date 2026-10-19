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

    constexpr std::size_t MinPathBytes = 1;

    // path_length and  pageId = 2* sizeof(std::uint32_t)
}

std::size_t PageBeforeImage::size() const
{
    return 4 + 4 + encodedPath(relativeFilePath).size() + PAGE_SIZE + JournalRecordHeaderBytes; // path length + pageId + path +  page size + record header
}
std::size_t FileBeforeImage::size() const
{
    return 4 + 8 + 1 + encodedPath(relativeFilePath).size() + JournalRecordHeaderBytes; // path length + original size + new file flag + path + record header
}

JournalFile::JournalFile(const LinuxDirectory &parent, const std::filesystem::path &path, const uuid_t &dbId, const uuid_t &statementId, LinuxFile::OpenMode mode)
    : file_(parent, path, mode), appendOffset_(mode == LinuxFile::OpenMode::CreateNew ? JournalDataOffset : file_.size()), writeSequenceInitialized_(mode == LinuxFile::OpenMode::CreateNew)
{
    uuid_copy(dbId_, dbId);
    uuid_copy(statementId_, statementId);
}

std::optional<JournalBoundary> JournalFile::readJournalBoundary(const std::uint8_t slot)
{
    std::array<std::byte, JournalBoundaryBytes> headerBytes{};
    const auto recordStart = JournalHeaderBytes + (slot * JournalBoundaryBytes);
    file_.readExactAt(recordStart, headerBytes);
    ByteReader reader(headerBytes);
    JournalBoundary header{};

    header.generation = reader.readUnsigned<std::uint64_t>();
    header.endOffset = reader.readUnsigned<std::uint64_t>();

    header.checksum = reader.readUnsigned<std::uint32_t>();

    std::vector<std::byte> bytes(JournalBoundaryBytes);
    file_.readExactAt(recordStart,
                      std::span<std::byte>{bytes});

    constexpr std::size_t checksumOffset = 8 + 8;

    std::fill_n(
        bytes.begin() + checksumOffset,
        sizeof(std::uint32_t),
        std::byte{0});

    std::vector<std::byte> checksumInput;
    checksumInput.reserve(
        sizeof(dbId_) + sizeof(statementId_) + bytes.size());

    for (const auto &id : {std::as_bytes(std::span{dbId_}),
                           std::as_bytes(std::span{statementId_})})
    {
        checksumInput.insert(checksumInput.end(), id.begin(), id.end());
    }

    checksumInput.insert(checksumInput.end(), bytes.begin(), bytes.end());
    if (checksum(checksumInput) != header.checksum)
    {
        return std::nullopt;
    }

    // readOffset_ = JournalDataOffset;
    // nextReadSequence_ = 0;
    return header;
}
std::uint64_t JournalFile::validateJournalHeader(uuid_t &statementId)
{
    const auto header = readJournalHeader();
    uuid_copy(statementId_, header.statementId);

    const auto slot0 = readJournalBoundary(0);
    const auto slot1 = readJournalBoundary(1);
    if (slot0 && slot1 &&
        slot0->generation == slot1->generation &&
        slot0->endOffset != slot1->endOffset)
    {
        throw std::runtime_error(
            "Conflicting journal boundaries");
    }

    auto selected = slot0;
    if (slot1 &&
        (!selected || slot1->generation > selected->generation))
    {
        selected = slot1;
    }

    if (!selected)
        throw std::runtime_error("No valid journal boundary");

    uuid_copy(statementId, header.statementId);
    return selected->endOffset;
}
std::uint64_t JournalFile::findRecoveryEnd(
    std::uint64_t fileSize)
{
    std::uint64_t offset{JournalDataOffset};
    (void)readJournalHeader(); // Validate the header and reset record iteration.
    while (readRecord(offset, fileSize))
    {
        // offset is updated by readRecord to the end of the record.
    }
    return offset;
}

JournalHeader JournalFile::readJournalHeader()
{

    std::array<std::byte, JournalHeaderBytes> headerBytes{};
    file_.readExactAt(0, headerBytes);
    ByteReader reader(headerBytes);
    JournalHeader header{};

    reader.readBytes(header.magic.data(), header.magic.size());
    if (header.magic != JournalMagic)
    {
        throw std::runtime_error("Invalid journal header magic");
    }

    header.version = reader.readUnsigned<std::uint32_t>();
    if (header.version != 1)
    {
        throw std::runtime_error("Unsupported journal version");
    }
    header.headerLength = reader.readUnsigned<std::uint32_t>();
    if (header.headerLength != JournalHeaderBytes)
    {
        throw std::runtime_error("Invalid journal header length");
    }

    reader.readBytes(header.dbId, sizeof(header.dbId));
    if (uuid_compare(dbId_, header.dbId) != 0)
    {
        throw std::runtime_error("Mismatch in db id");
    }

    reader.readBytes(header.statementId, sizeof(header.statementId));

    header.pageSize = reader.readUnsigned<std::uint64_t>();
    if (header.pageSize != PAGE_SIZE)
    {
        throw std::runtime_error("Unsupported journal page size");
    }
    header.checksum = reader.readUnsigned<std::uint32_t>();

    const auto content = std::span<const std::byte>{headerBytes}.first(JournalHeaderBytes - sizeof(header.checksum));
    if (checksum(content) != header.checksum)
    {
        throw std::runtime_error("Invalid journal header checksum");
    }

    // uuid_copy(dbId_, header.dbId);

    readOffset_ = JournalDataOffset;
    nextReadSequence_ = 0;
    return header;
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

    // [type:u8][record size:u64][sequence:u32][checksum:u32]
    std::array<std::byte, JournalRecordHeaderBytes> headerBytes{};
    file_.readExactAt(recordStart, headerBytes);
    ByteReader prefixReader(headerBytes);

    const auto journalType = static_cast<JournalRecordType>(
        prefixReader.readUnsigned<std::uint8_t>());
    const auto recordSize = prefixReader.readUnsigned<std::uint64_t>();
    const auto minFileRecord = JournalRecordHeaderBytes + 4 + MinPathBytes + 8 + 1;
    const auto minPageRecord = JournalRecordHeaderBytes + 4 + MinPathBytes + 4 + PAGE_SIZE;
    if (journalType == JournalRecordType::FileBeforeImage &&
        (recordSize < minFileRecord || recordSize > maxFileRecord))
    {
        throw std::runtime_error("Invalid file journal record size");
    }
    if (journalType == JournalRecordType::PageBeforeImage &&
        (recordSize < minPageRecord || recordSize > maxPageRecord))
    {
        throw std::runtime_error("Invalid page journal record size");
    }
    if (journalType != JournalRecordType::FileBeforeImage && journalType != JournalRecordType::PageBeforeImage)
    {
        throw std::runtime_error("invalid Journal Record Type");
    }
    const auto sequenceNumber = prefixReader.readUnsigned<std::uint32_t>();
    const auto storedChecksum = prefixReader.readUnsigned<std::uint32_t>();
    JournalRecordHeader header{
        .type = journalType,
        .recordSize = recordSize,
        .sequenceNumber = sequenceNumber,
        .checksum = storedChecksum};

    if (recordSize > fileSize - recordStart)
    {
        throw std::runtime_error("Truncated journal record");
    }

    std::vector<std::byte> bytes(recordSize);
    file_.readExactAt(recordStart,
                      std::span<std::byte>{bytes});

    constexpr std::size_t checksumOffset = 1 + 8 + 4;

    std::fill_n(
        bytes.begin() + checksumOffset,
        sizeof(std::uint32_t),
        std::byte{0});

    std::vector<std::byte> checksumInput;
    checksumInput.reserve(
        sizeof(dbId_) + sizeof(statementId_) + bytes.size());

    for (const auto &id : {std::as_bytes(std::span{dbId_}),
                           std::as_bytes(std::span{statementId_})})
    {
        checksumInput.insert(checksumInput.end(), id.begin(), id.end());
    }

    checksumInput.insert(checksumInput.end(), bytes.begin(), bytes.end());

    if (checksum(checksumInput) != storedChecksum)
    {
        throw std::runtime_error("Invalid journal record checksum");
    }

    if (sequenceNumber != nextReadSequence_)
    {
        throw std::runtime_error("Unexpected journal record sequence");
    }

    ByteReader reader(bytes);
    reader.seek(JournalRecordHeaderBytes);

    JournalRecord result{};
    result.header = header;

    if (journalType == JournalRecordType::FileBeforeImage)
    {
        auto &record = result.record.emplace<FileBeforeImage>();

        record.relativeFilePath = reader.readString();
        (void)encodedPath(record.relativeFilePath);
        record.originalSize = reader.readUnsigned<std::uint64_t>();

        const auto newFile = reader.readUnsigned<std::uint8_t>();
        if (newFile > 1)
        {
            throw std::runtime_error("Invalid journal new-file flag");
        }
        record.newFile = newFile != 0;
    }
    else // Type was already validated as PageBeforeImage.
    {
        auto &record = result.record.emplace<PageBeforeImage>();

        record.relativeFilePath = reader.readString();
        (void)encodedPath(record.relativeFilePath);
        record.pageId = reader.readUnsigned<std::uint32_t>();
        reader.readBytes(record.originalPage.data(), record.originalPage.size());
    }

    if (reader.position() != reader.size())
    {
        throw std::runtime_error("Unexpected trailing bytes in journal record");
    }

    offset = recordStart + recordSize;
    ++nextReadSequence_;
    return result;
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
    return appendRecord(JournalRecordType::FileBeforeImage, writer.bytes());
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
    return appendRecord(JournalRecordType::PageBeforeImage, writer.bytes());
}

void JournalFile::writeJournalBoundary(const JournalBoundary &boundary, const std::uint8_t slot)
{
    ByteWriter writer(JournalBoundaryBytes);
    writer.writeUnsigned<std::uint64_t>(boundary.generation);
    writer.writeUnsigned<std::uint64_t>(boundary.endOffset);

    const auto checksumOffset = writer.position(); // Byte 13 within this buffer
    writer.writeUnsigned<std::uint32_t>(boundary.checksum);

    std::vector<std::byte> bytes;
    bytes.reserve(sizeof(dbId_) + sizeof(statementId_) + writer.bytes().size());
    for (const auto &id : {std::as_bytes(std::span{dbId_}),
                           std::as_bytes(std::span{statementId_})})
    {
        bytes.insert(bytes.end(), id.begin(), id.end());
    }

    bytes.insert(bytes.end(), writer.bytes().begin(), writer.bytes().end());

    const auto recordChecksum = checksum(bytes);
    writer.writeUnsignedAt<std::uint32_t>(
        checksumOffset, recordChecksum);

    if (writer.size() > std::numeric_limits<std::uint64_t>::max() - appendOffset_)
    {
        throw std::out_of_range("Journal append offset overflow");
    }
    try
    {
        file_.writeAllAt(JournalHeaderBytes + (slot * JournalBoundaryBytes), writer.bytes());
    }
    catch (...)
    {
        // A partial record may have reached disk. Never append past it on this
        // object or report it as a successfully synchronized record stream.
        appendFailed_ = true;
        throw;
    }
    // return writer.position();
}

void JournalFile::writeJournalHeader(const JournalHeader &header)
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }
    ByteWriter writer(JournalHeaderBytes);

    writer.writeBytes(JournalMagic.data(), JournalMagic.size());

    writer.writeUnsigned<std::uint32_t>(header.version);
    writer.writeUnsigned<std::uint32_t>(header.headerLength);
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
        file_.writeAllAt(0, writer.bytes());
    }
    catch (...)
    {
        // A partial record may have reached disk. Never append past it on this
        // object or report it as a successfully synchronized record stream.
        appendFailed_ = true;
        throw;
    }
    // appendOffset_ += writer.size();
    // return appendOffset_;
}

void JournalFile::sync()
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }
    file_.sync();
}

std::uint64_t JournalFile::appendRecord(
    JournalRecordType type,
    std::span<const std::byte> payload)
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }

    if (!writeSequenceInitialized_)
    {
        // Resume only a fully validated record stream. Preserve any independent
        // read cursor, including its expected sequence, even if validation fails.
        const auto savedReadOffset = readOffset_;
        const auto savedReadSequence = nextReadSequence_;
        try
        {
            if (file_.size() != appendOffset_)
                throw std::runtime_error("Journal size changed before append");
            (void)findRecoveryEnd(appendOffset_);
            nextWriteSequence_ = nextReadSequence_;
            writeSequenceInitialized_ = true;
        }
        catch (...)
        {
            readOffset_ = savedReadOffset;
            nextReadSequence_ = savedReadSequence;
            throw;
        }
        readOffset_ = savedReadOffset;
        nextReadSequence_ = savedReadSequence;
    }

    if (payload.size() >
        std::numeric_limits<std::size_t>::max() - JournalRecordHeaderBytes)
    {
        throw std::out_of_range("Journal record size overflow");
    }

    if (nextWriteSequence_ > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::overflow_error("Journal sequence exhausted");
    }
    const auto recordSize = JournalRecordHeaderBytes + payload.size();
    ByteWriter writer(recordSize);

    writer.writeUnsigned<std::uint8_t>(
        static_cast<std::uint8_t>(type));
    writer.writeUnsigned<std::uint64_t>(recordSize);
    writer.writeUnsigned<std::uint32_t>(
        static_cast<std::uint32_t>(nextWriteSequence_));

    const auto checksumOffset = writer.position(); // Byte 13 within this buffer
    writer.writeUnsigned<std::uint32_t>(0);

    writer.writeBytes(payload.data(), payload.size());

    std::vector<std::byte> bytes;
    bytes.reserve(sizeof(dbId_) + sizeof(statementId_) + writer.bytes().size());
    for (const auto &id : {std::as_bytes(std::span{dbId_}),
                           std::as_bytes(std::span{statementId_})})
    {
        bytes.insert(bytes.end(), id.begin(), id.end());
    }

    bytes.insert(bytes.end(), writer.bytes().begin(), writer.bytes().end());

    const auto recordChecksum = checksum(bytes);
    writer.writeUnsignedAt<std::uint32_t>(
        checksumOffset, recordChecksum);
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
        appendFailed_ = true;
        throw;
    }
    appendOffset_ += writer.size();
    ++nextWriteSequence_;
    return appendOffset_;
}
