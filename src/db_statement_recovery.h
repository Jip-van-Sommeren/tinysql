#pragma once
#include "db_journal_file.h"
#include "db_page_file.h"
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <uuid/uuid.h>

class StatementRecovery
{
private:
    friend class BufferManager;
    // Shared with a buffer manager so pending writes survive its destruction.
    // Each handle must synchronize the same file descriptor that it wrote.
    struct Participant
    {
        std::filesystem::path relativePath;
        std::uint64_t generation = 0;
        std::unordered_set<std::uint32_t> dirtyPages;
        bool needsSync = false;
    };
    void trackParticipant(const std::shared_ptr<Participant> &participant);

public:
    explicit StatementRecovery(const std::filesystem::path &path, const uuid_t &dbId);

    void begin();
    void initializeForRecover();
    void capturePageOnce(const PageBeforeImage &image);
    void ensureDurable();
    void ensureDurable(std::uint64_t requiredEnd);
    void ensureDurable(JournalCoverage coverage);
    void requireReadable() const;
    std::uint64_t cacheEpoch() const { return cacheEpoch_; }

    void statementFailed() { failed_ = true; }

    void rollback();
    bool isActive() const { return active_; }
    void captureFileOnce(const std::filesystem::path &relativePath, bool newFile = false);
    bool isNewFile(const std::filesystem::path &relativePath) const;
    bool hasCapturedPage(const std::filesystem::path &path, std::uint32_t pageId) const;
    std::uint64_t getOriginalFileSize(const std::filesystem::path &relativePath) const;
    JournalCoverage requiredCoverageForPage(const std::filesystem::path &relativePath, std::uint32_t pageId) const;
    void validateCoverage(const std::filesystem::path &relativePath, std::uint32_t pageId,
                          JournalCoverage coverage) const;
    std::uint64_t validateJournalForRecovery();

    void commit();

private:
    struct CapturedFile
    {
        FileBeforeImage beforeImage;
        JournalCoverage coverage;
    };
    std::optional<JournalFile> journalFile_;
    const std::filesystem::path dbRootPath_;
    const std::filesystem::path journalPath_;
    std::unordered_map<
        std::filesystem::path,
        std::unordered_map<std::uint32_t, JournalCoverage>>
        capturedPages_;
    std::unordered_map<std::filesystem::path, CapturedFile> originalFiles_;
    std::vector<std::shared_ptr<Participant>> participants_;

    bool journalDirectoryNeedsSync_ = false;
    bool failed_ = false;
    bool active_ = false;
    enum class Retirement
    {
        None,
        Commit,
        Rollback
    };
    Retirement retirement_ = Retirement::None;

    // Never reset generations when a statement finishes. Rollback also
    // invalidates all existing handles, including pinned pages and cursors.
    std::uint64_t generation_ = 0;
    std::uint64_t cacheEpoch_ = 0;

    std::uint64_t appendOffset_ = 0;
    std::uint64_t syncOffset_ = 0;
    std::uint64_t recoveryEnd_ = 0;
    uuid_t statementId_;
    uuid_t dbId_;
    bool recoveryReady_ = false;

    void requireWritableStatement() const;
    void finish();
};
