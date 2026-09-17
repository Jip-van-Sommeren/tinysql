#pragma once

#include "db_linux_file.h"
#include "db_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

struct PageBeforeImage
{
    std::filesystem::path relativeFilePath;
    std::uint32_t pageId;
    RawPage originalPage;

    std::size_t size() const;
};

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
    std::optional<PageBeforeImage> readNext();
    void rewind() noexcept { readOffset_ = 0; }

    // Convenience snapshot from offset zero; does not change readNext's cursor.
    // Preserves file paths, record order, and duplicate page IDs.
    std::vector<PageBeforeImage> readPages();

    void writePage(const PageBeforeImage &pageBeforeImage);
    void sync();

private:
    LinuxFile file_;
    std::uint64_t appendOffset_ = 0;
    std::uint64_t readOffset_ = 0;
    bool appendFailed_ = false;

    std::optional<PageBeforeImage> readRecord(
        std::uint64_t &offset, std::uint64_t fileSize);
};
