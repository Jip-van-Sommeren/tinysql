#pragma once

#include "db_linux_file.h"
#include "db_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>
#include <variant>
#include <uuid/uuid.h>

constexpr std::uint32_t JournalHeaderBytes =
    8 + 4 + 4 + 16 + 16 + 8 + 4; // 60 bytes

constexpr std::uint64_t JournalRecordHeaderBytes =
    1 + 8 + 4 + 4; // 17 bytes

constexpr std::array<char, 8> JournalMagic{
    'D', 'B', 'J', 'R', 'N', 'L', '\0', '\0'};

constexpr std::size_t JournalBoundaryBytes = 8 + 8 + 4;
constexpr std::size_t JournalDataOffset =
    JournalHeaderBytes + 2 * JournalBoundaryBytes;

struct JournalBoundary
{
    std::uint64_t generation; // Increments whenever a boundary is published.
    std::uint64_t endOffset;  // Exclusive end of the required journal prefix.

    std::uint32_t checksum;
};

struct JournalHeader
{
    std::array<char, 8> magic;
    std::uint32_t version;
    std::uint32_t headerLength; // Bytes in the complete serialized journal header.
    uuid_t dbId;
    uuid_t statementId;
    std::uint64_t pageSize;
    std::uint32_t checksum;
};

enum class JournalRecordType : std::uint8_t
{
    FileBeforeImage = 1,
    PageBeforeImage = 2,
};

struct JournalRecordHeader
{
    JournalRecordType type;
    std::uint64_t recordSize; // Bytes, including header and path.
    std::uint32_t sequenceNumber;
    std::uint32_t checksum;
};

struct PageBeforeImage
{
    std::filesystem::path relativeFilePath;
    std::uint32_t pageId;
    RawPage originalPage;

    std::size_t size() const;
};

struct FileBeforeImage
{
    std::filesystem::path relativeFilePath;
    std::uint64_t originalSize; // Bytes, including all pages.
    bool newFile = false;

    std::size_t size() const;
};

struct JournalRecord
{
    JournalRecordHeader header;
    std::variant<FileBeforeImage, PageBeforeImage> record;
};

// using JournalRecord = std::variant<FileBeforeImage, PageBeforeImage>;

// Record I/O only, not yet a complete statement recovery protocol. Callers must
// resolve old journals before starting a new statement or applying undo records.
class JournalFile
{
public:
    // New journals are created exclusively. Reopening must be explicit.
    explicit JournalFile(const LinuxDirectory &parent,
                         const std::filesystem::path &path,
                         const uuid_t &dbId, const uuid_t &statementId,
                         LinuxFile::OpenMode mode = LinuxFile::OpenMode::CreateNew);

    static constexpr std::size_t MaxPathBytes = 4096;
    static constexpr std::size_t maxFileRecord =
        JournalRecordHeaderBytes + 4 + MaxPathBytes + 8 + 1;

    static constexpr std::size_t maxPageRecord =
        JournalRecordHeaderBytes + 4 + MaxPathBytes + 4 + PAGE_SIZE;

    // nullopt means EOF between records; truncated records throw.

    JournalHeader readJournalHeader();
    std::optional<JournalRecord> readNext();
    std::optional<JournalRecord> readNext(std::uint64_t end);

    std::uint64_t findRecoveryEnd(
        std::uint64_t fileSize);
    void rewind() noexcept
    {
        readOffset_ = JournalDataOffset;
        nextReadSequence_ = 0;
    }
    void rewind(std::uint64_t start) { readOffset_ = start; }

    // Appends return the end of the complete record; neither synchronizes it.
    std::uint64_t writeFileBeforeImage(const FileBeforeImage &fileBeforeImage);

    std::uint64_t writePage(const PageBeforeImage &pageBeforeImage);
    void sync();
    std::uint64_t size() const { return file_.size(); }

    void writeJournalHeader(const JournalHeader &header);
    void writeJournalBoundary(const JournalBoundary &boundary, const std::uint8_t slot);

    std::uint64_t validateJournalHeader(uuid_t &statementId);
    std::optional<JournalBoundary> readJournalBoundary(const std::uint8_t slot);

private:
    LinuxFile file_;
    std::uint64_t appendOffset_{};
    std::uint64_t readOffset_{0};
    std::uint64_t nextWriteSequence_{0};
    std::uint64_t nextReadSequence_{0};

    uuid_t dbId_;
    uuid_t statementId_;
    bool writeSequenceInitialized_ = false;
    bool appendFailed_ = false;

    std::optional<JournalRecord> readRecord(
        std::uint64_t &offset, std::uint64_t fileSize);

    std::uint64_t appendRecord(
        JournalRecordType type,
        std::span<const std::byte> payload);
};
