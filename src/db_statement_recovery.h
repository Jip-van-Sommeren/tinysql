#pragma once
#include "db_journal_file.h"
#include "db_page_file.h"
#include <unordered_map>
#include <unordered_set>

class StatementRecovery
{
public:
    explicit StatementRecovery(const std::filesystem::path &path,
                               LinuxFile::OpenMode mode = LinuxFile::OpenMode::CreateNew);

    void begin();
    void capturePageOnce(const PageBeforeImage &image);
    void ensureDurable();
    void ensureDurable(std::uint64_t requiredEnd);

    void statementFailed() { failed_ = true; }

    void rollback();
    bool isActive();
    void captureFileOnce(const std::filesystem::path &relativePath, const bool newFile);
    bool hasCapturedPage(const std::filesystem::path &path, std::uint32_t pageId) const;
    const std::uint64_t getOriginalFileSize(const std::filesystem::path &relativePath);
    void commit();

private:
    std::optional<JournalFile> journalFile_;
    const std::filesystem::path dbRootPath_;
    const std::filesystem::path journalPath_;
    std::unordered_map<
        std::filesystem::path,
        std::unordered_set<std::uint32_t>>
        capturedPages_;
    std::unordered_map<std::filesystem::path, std::uint64_t>
        originalFileSizes_;

    bool journalDirectoryNeedsSync_ = false;
    bool failed_ = false;
    bool active_ = false;

    std::uint64_t appendOffset_;
    std::uint64_t syncOffset_;
};