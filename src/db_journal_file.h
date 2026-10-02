#pragma once

#include "db_linux_file.h"
#include "db_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>
#include <variant>

enum class JournalRecordType : std::uint8_t
{
    FileBeforeImage = 1,
    PageBeforeImage = 2,
};

struct JournalRecordHeader
{
    JournalRecordType type;
    std::uint64_t recordSize; // Bytes, including header and path.

    std::uint32_t pathLength; // Bytes, excluding null terminator.
    std::uint32_t statementId;   // For future use; currently always 0.
    std::uint32_t version; // For future use; currently always 0.
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
};

struct CapturedFile
{
    FileBeforeImage beforeImage;
    std::uint64_t requiredEnd;
};

using JournalRecord = std::variant<FileBeforeImage, PageBeforeImage>;

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
    std::optional<JournalRecord> readNext();
    void rewind() noexcept { readOffset_ = 0; }

    // Appends return the end of the complete record; neither synchronizes it.
    std::uint64_t writeFileBeforeImage(const FileBeforeImage &fileBeforeImage);

    std::uint64_t writePage(const PageBeforeImage &pageBeforeImage);
    void sync();
    std::uint64_t size() const { return file_.size(); }

private:
    LinuxFile file_;
    std::uint64_t appendOffset_ = 0;
    std::uint64_t readOffset_ = 0;
    bool appendFailed_ = false;

    std::optional<JournalRecord> readRecord(
        std::uint64_t &offset, std::uint64_t fileSize);
};
