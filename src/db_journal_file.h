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
    1 + 8 + 4 + 4; // 21 bytes

constexpr std::array<char, 8> JournalMagic{
    'D', 'B', 'J', 'R', 'N', 'L', '\0', '\0'};

struct JournalHeader
{
    std::array<char, 8> magic;
    std::uint32_t version;
    std::uint32_t headerLength; // Bytes, including magic, version, epoch, generation, checksum.
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
    explicit JournalFile(
        const std::filesystem::path &path,
        LinuxFile::OpenMode mode = LinuxFile::OpenMode::CreateNew);

    static constexpr std::size_t MaxPathBytes = 4096;

    // nullopt means EOF between records; truncated records throw.

    JournalHeader readJournalHeader();
    std::optional<JournalRecord> readNext();
    std::optional<JournalRecord> readNext(std::uint64_t end);

    std::uint64_t findRecoveryEnd(
        std::uint64_t fileSize);
    void rewind() noexcept { readOffset_ = 0; }

    // Appends return the end of the complete record; neither synchronizes it.
    std::uint64_t writeFileBeforeImage(const FileBeforeImage &fileBeforeImage);

    std::uint64_t writePage(const PageBeforeImage &pageBeforeImage);
    void sync();
    std::uint64_t size() const { return file_.size(); }

    std::uint64_t writeJournalHeader(const JournalHeader &header);

private:
    LinuxFile file_;
    std::uint64_t appendOffset_{0};
    std::uint64_t readOffset_{0};
    std::uint32_t nextWriteSequence_{0};
    bool appendFailed_ = false;

    std::optional<JournalRecord> readRecord(
        std::uint64_t &offset, std::uint64_t fileSize);

    std::uint64_t appendRecord(
        JournalRecordType type,
        std::span<const std::byte> payload);
};
