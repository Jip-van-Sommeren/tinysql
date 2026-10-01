#pragma once
#include "db_journal_file.h"
#include "db_page_file.h"
#include <unordered_map>
#include <unordered_set>

class StatementRecovery
{
public:
    explicit StatementRecovery(const std::filesystem::path &path);

    void begin();
    void capturePageOnce(const PageBeforeImage &image);
    void ensureDurable();
    void ensureDurable(std::uint64_t requiredEnd);

    void statementFailed() { failed_ = true; }

    void rollback();
    bool isActive() const { return active_; }
    void captureFileOnce(const std::filesystem::path &relativePath, bool newFile = false);
    bool isNewFile(const std::filesystem::path &relativePath) const;
    bool hasCapturedPage(const std::filesystem::path &path, std::uint32_t pageId) const;
    std::uint64_t getOriginalFileSize(const std::filesystem::path &relativePath) const;
    std::uint64_t requiredEndForPage(const std::filesystem::path &relativePath, std::uint32_t pageId) const;
    void unSetNeedsSync(const std::filesystem::path &relativePath)
    {
        if (needSyncMap_.contains(relativePath))
        {
            needSyncMap_[relativePath] = false;
        }
        else
        {
            throw std::logic_error("File should be in statementrecovery");
        }
    }

    void setNeedsSync(const std::filesystem::path &relativePath)
    {
        if (needSyncMap_.contains(relativePath))
        {
            needSyncMap_[relativePath] = true;
        }
        else
        {
            throw std::logic_error("File should be in statementrecovery");
        }
    }
    void commit();

private:
    std::optional<JournalFile> journalFile_;
    const std::filesystem::path dbRootPath_;
    const std::filesystem::path journalPath_;
    std::unordered_map<
        std::filesystem::path,
        std::unordered_map<std::uint32_t, std::uint64_t>>
        capturedPages_;
    std::unordered_map<std::filesystem::path, CapturedFile> originalFiles_;
    std::unordered_map<std::filesystem::path, bool> needSyncMap_;

    bool journalDirectoryNeedsSync_ = false;
    bool failed_ = false;
    bool active_ = false;

    std::uint64_t appendOffset_ = 0;
    std::uint64_t syncOffset_ = 0;

    void requireWritableStatement() const;
    void finish();
};
