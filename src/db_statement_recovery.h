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
    void ensureDurable(std::uint32_t requiredEnd);

    void rollback();
    bool isActive();
    bool hasCapturedPage(const std::filesystem::path &path, std::uint32_t pageId) const;

    void commit();

private:
    std::optional<JournalFile> journalFile_;
    const std::filesystem::path dbRootPath_;
    const std::filesystem::path journalPath_;
    std::unordered_map<
        std::filesystem::path,
        std::unordered_set<std::uint32_t>>
        capturedPages_;
    bool journalDirectoryNeedsSync_ = false;
    bool failed_ = false;
};