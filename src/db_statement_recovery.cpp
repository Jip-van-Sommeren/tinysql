#include "db_statement_recovery.h"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <zlib.h>

namespace
{

    std::uint32_t checksum(std::span<const std::byte> bytes)
    {
        return static_cast<std::uint32_t>(
            crc32_z(0,
                    reinterpret_cast<const Bytef *>(bytes.data()),
                    bytes.size()));
    }
    // path_length and  pageId = 2* sizeof(std::uint32_t)
}

StatementRecovery::StatementRecovery(const std::filesystem::path &path, const uuid_t &dbId)
    : dbRootPath_(path), dbId_(*dbId), journalPath_(path / "journal" / "journal.log") {}

void StatementRecovery::requireWritableStatement() const
{
    requireReadable();
    if (!active_ || !journalFile_)
    {
        throw std::logic_error("No active statement");
    }
}

void StatementRecovery::requireReadable() const
{
    if (failed_)
    {
        throw std::runtime_error("Statement has failed; recovery is required");
    }
}

std::uint64_t StatementRecovery::validateJournalForRecovery()
{

    if (!journalFile_)
    {
        throw std::logic_error("No journal open for recovery");
    }

    const auto end = journalFile_->findRecoveryEnd(journalFile_->size());
    journalFile_->rewind();

    std::unordered_map<std::filesystem::path, FileBeforeImage> rollbackFiles;
    while (auto image = journalFile_->readNext(end))
    {
        std::visit(
            [&](const auto &value)
            {
                using T = std::decay_t<decltype(value)>;

                if constexpr (std::is_same_v<T, PageBeforeImage>)
                {
                    const auto metadata = rollbackFiles.find(value.relativeFilePath);
                    if (metadata == rollbackFiles.end() || metadata->second.newFile ||
                        std::uint64_t{value.pageId} * PAGE_SIZE >= metadata->second.originalSize)
                    {
                        throw std::runtime_error("Page before-image has no valid original file metadata");
                    }
                }
                else if constexpr (std::is_same_v<T, FileBeforeImage>)
                {
                    if (value.originalSize % PAGE_SIZE != 0 ||
                        (value.newFile && value.originalSize != 0))
                    {
                        throw std::runtime_error("Invalid original file metadata in journal");
                    }
                    auto [it, inserted] = rollbackFiles.try_emplace(value.relativeFilePath, value);
                    if (!inserted && (it->second.originalSize != value.originalSize ||
                                      it->second.newFile != value.newFile))
                    {
                        throw std::runtime_error("Conflicting original file metadata in journal");
                    }
                }
            },
            image.value().record);
    }
    journalFile_->rewind();
    return end;
}

void StatementRecovery::initializeForRecover()
{
    if (journalFile_ || active_ || failed_)
        throw std::logic_error("A statement is already active");
    failed_ = true; // No normal access until replay and cleanup finish.
    journalFile_.emplace(
        journalPath_,
        LinuxFile::OpenMode::OpenExisting);
    recoveryEnd_ = validateJournalForRecovery();
    recoveryReady_ = true;
    const auto end = journalFile_->size();

    journalFile_->sync();

    LinuxDirectory directory{journalPath_.parent_path()};
    directory.sync();

    // Update these only after both syncs succeed.
    appendOffset_ = end;
    syncOffset_ = end;
    journalDirectoryNeedsSync_ = false;
    active_ = true;
}

void StatementRecovery::begin()
{
    if (journalFile_ || active_ || failed_)
    {
        throw std::logic_error("A statement is already active");
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Statement generation exhausted");

    journalFile_.emplace(
        journalPath_,
        LinuxFile::OpenMode::CreateNew);
    ++generation_;
    recoveryReady_ = true;
    appendOffset_ = 0;
    syncOffset_ = 0;
    journalDirectoryNeedsSync_ = true;
    active_ = true;
    uuid_generate_random(statementId_);

    journalFile_->writeJournalHeader(
        JournalHeader{
            .magic = JournalMagic,
            .version = 1,
            .headerLength = JournalHeaderBytes,
            .dbId = *dbId_,
            .statementId = *statementId_,
            .pageSize = PAGE_SIZE,
            .checksum = 0});
    journalFile_->sync();
}

void StatementRecovery::capturePageOnce(const PageBeforeImage &image)
{
    requireWritableStatement();
    try
    {
        if (hasCapturedPage(image.relativeFilePath, image.pageId))
            return;
        if (std::uint64_t{image.pageId} * PAGE_SIZE >= getOriginalFileSize(image.relativeFilePath))
            throw std::logic_error("Page before-image is outside the original file");
        appendOffset_ = journalFile_->writePage(image);
        recoveryEnd_ = appendOffset_;
        capturedPages_[image.relativeFilePath].emplace(image.pageId, JournalCoverage{generation_, appendOffset_});
    }
    catch (...)
    {
        failed_ = true;
        throw;
    }
}
bool StatementRecovery::hasCapturedPage(const std::filesystem::path &path, std::uint32_t pageId) const
{
    if (failed_)
    {
        throw std::runtime_error(
            "Statement has failed; recovery is required");
    }
    const auto file = capturedPages_.find(path);

    return file != capturedPages_.end() &&
           file->second.contains(pageId);
}

void StatementRecovery::captureFileOnce(const std::filesystem::path &relativePath, bool newFile)
{
    requireWritableStatement();
    try
    {
        const auto resolvedPath = dbRootPath_ / relativePath;
        if (newFile && std::filesystem::exists(std::filesystem::symlink_status(resolvedPath)))
        {
            throw std::runtime_error("Table already exists: " + resolvedPath.string());
        }
        if (const auto existing = originalFiles_.find(relativePath); existing != originalFiles_.end())
        {
            if (newFile && !existing->second.beforeImage.newFile)
                throw std::logic_error("Cannot recreate a file that existed before the statement");
            return;
        }

        // A new file does not exist yet; opening it here would violate write-ahead ordering.
        const auto originalSize = newFile ? std::uint64_t{0} : PageFile(resolvedPath).size();
        if (originalSize % PAGE_SIZE != 0)
            throw std::runtime_error("Invalid original table file size");

        const FileBeforeImage image{
            .relativeFilePath = relativePath, .originalSize = originalSize, .newFile = newFile};
        appendOffset_ = journalFile_->writeFileBeforeImage(image);
        recoveryEnd_ = appendOffset_;
        originalFiles_.emplace(relativePath, CapturedFile{.beforeImage = image, .coverage = {generation_, appendOffset_}});
    }
    catch (...)
    {
        failed_ = true;
        throw;
    }
}

bool StatementRecovery::isNewFile(const std::filesystem::path &relativePath) const
{
    requireWritableStatement();
    const auto file = originalFiles_.find(relativePath);
    return file != originalFiles_.end() && file->second.beforeImage.newFile;
}

JournalCoverage StatementRecovery::requiredCoverageForPage(const std::filesystem::path &relativePath, std::uint32_t pageId) const
{
    requireWritableStatement();

    const auto &file = originalFiles_.at(relativePath);
    const auto pageOffset = std::uint64_t{pageId} * PAGE_SIZE;

    // Newly appended pages have no original contents to restore.
    // For a newly created file, originalSize is zero.
    if (pageOffset >= file.beforeImage.originalSize)
    {
        return file.coverage;
    }

    // Existing pages require both file metadata and a page before-image.
    // at() throws if the required capture is missing.
    const auto pageCoverage = capturedPages_.at(relativePath).at(pageId);

    return {generation_, std::max(file.coverage.end, pageCoverage.end)};
}

void StatementRecovery::validateCoverage(const std::filesystem::path &relativePath,
                                         std::uint32_t pageId, JournalCoverage coverage) const
{
    requireWritableStatement();
    if (coverage.generation != generation_)
        throw std::logic_error("Stale journal coverage generation");
    const auto required = requiredCoverageForPage(relativePath, pageId);
    if (coverage.end != required.end)
        throw std::logic_error("Page journal coverage does not match its before-image");
}

void StatementRecovery::trackParticipant(const std::shared_ptr<Participant> &participant)
{
    requireWritableStatement();
    (void)getOriginalFileSize(participant->relativePath);
    if (participant->generation != generation_)
    {
        participants_.push_back(participant);
        participant->generation = generation_;
        participant->dirtyPages.clear();
        participant->needsSync = false;
    }
}

std::uint64_t StatementRecovery::getOriginalFileSize(const std::filesystem::path &relativePath) const
{
    requireWritableStatement();
    if (!originalFiles_.contains(relativePath))
    {
        throw std::logic_error(
            "Original file size was not captured: " +
            relativePath.string());
    }

    return originalFiles_.at(relativePath).beforeImage.originalSize;
}

void StatementRecovery::rollback()
{
    if (!journalFile_)
    {
        throw std::logic_error("No active journal");
    }
    if (retirement_ == Retirement::Commit)
        throw std::runtime_error("Commit outcome is uncertain; reopen the database before recovery");
    failed_ = true;
    if (!recoveryReady_)
        throw std::runtime_error("Journal has not been validated for recovery");
    if (retirement_ == Retirement::Rollback)
    {
        LinuxDirectory directory{journalPath_.parent_path()};
        directory.sync();
        finish();
        return;
    }
    if (cacheEpoch_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Cache epoch exhausted");
    std::unordered_map<std::filesystem::path, PageFile> openedFiles;
    std::unordered_map<std::filesystem::path, FileBeforeImage> rollbackFiles;

    journalFile_->rewind();

    const auto header = journalFile_->readJournalHeader(); // Skip header; it is not needed for recovery.

    while (auto image = journalFile_->readNext(recoveryEnd_))
    {
        std::visit(
            [&](const auto &value)
            {
                using T = std::decay_t<decltype(value)>;

                if constexpr (std::is_same_v<T, PageBeforeImage>)
                {
                    const auto metadata = rollbackFiles.find(value.relativeFilePath);
                    if (metadata == rollbackFiles.end() || metadata->second.newFile ||
                        std::uint64_t{value.pageId} * PAGE_SIZE >= metadata->second.originalSize)
                    {
                        throw std::runtime_error("Page before-image has no valid original file metadata");
                    }

                    const std::filesystem::path resolvedTablePath =
                        dbRootPath_ / value.relativeFilePath;

                    auto [it, inserted] = openedFiles.try_emplace(
                        value.relativeFilePath,
                        resolvedTablePath,
                        LinuxFile::OpenMode::OpenExisting);

                    PageFile &file = it->second;

                    file.writePage(
                        value.pageId,
                        value.originalPage);
                }
                else if constexpr (std::is_same_v<T, FileBeforeImage>)
                {
                    if (value.originalSize % PAGE_SIZE != 0 ||
                        (value.newFile && value.originalSize != 0))
                    {
                        throw std::runtime_error("Invalid original file metadata in journal");
                    }
                    auto [it, inserted] = rollbackFiles.try_emplace(value.relativeFilePath, value);
                    if (!inserted && (it->second.originalSize != value.originalSize ||
                                      it->second.newFile != value.newFile))
                    {
                        throw std::runtime_error("Conflicting original file metadata in journal");
                    }
                }
            },
            image.value().record);
    }

    std::unordered_set<std::filesystem::path> changedDirectories;
    for (const auto &[relativePath, metadata] : rollbackFiles)
    {
        if (metadata.newFile)
        {
            // Already absent is success: creation or a previous rollback may have failed.
            (void)std::filesystem::remove(dbRootPath_ / relativePath);
            changedDirectories.insert((dbRootPath_ / relativePath).parent_path());
            continue;
        }
        auto [it, inserted] = openedFiles.try_emplace(
            relativePath,
            dbRootPath_ / relativePath,
            LinuxFile::OpenMode::OpenExisting);

        it->second.resize(metadata.originalSize);
        it->second.sync();
    }
    for (const auto &path : changedDirectories)
    {
        LinuxDirectory directory{path};
        directory.sync();
    }
    if (!std::filesystem::remove(journalPath_))
    {
        throw std::runtime_error("Expected journal file was missing");
    }
    retirement_ = Retirement::Rollback;
    ++cacheEpoch_;
    LinuxDirectory directory{journalPath_.parent_path()};
    directory.sync();

    finish();
}

void StatementRecovery::commit()
{
    requireWritableStatement();
    try
    {
        // Do not reopen paths to guess at durability. Acknowledgements must
        // come from the exact handles that issued writes, even if now closed.
        for (const auto &participant : participants_)
        {
            if (!participant->dirtyPages.empty())
                throw std::logic_error("Cannot commit with unflushed database pages");
            if (participant->needsSync)
                throw std::logic_error("Cannot commit with unsynchronized database writes");
        }
        // Table mutation methods synchronize file contents before commit. Persist
        // new names too, including callers using StorageEngine directly.
        std::unordered_set<std::filesystem::path> changedDirectories;
        for (const auto &[relativePath, metadata] : originalFiles_)
        {
            if (metadata.beforeImage.newFile)
                changedDirectories.insert((dbRootPath_ / relativePath).parent_path());
        }
        for (const auto &path : changedDirectories)
        {
            LinuxDirectory directory{path};
            directory.sync();
        }
        if (!std::filesystem::remove(journalPath_))
            throw std::runtime_error("Expected journal file was missing");
        retirement_ = Retirement::Commit;
        LinuxDirectory directory{journalPath_.parent_path()};
        directory.sync();
    }
    catch (...)
    {
        failed_ = true;
        throw;
    }
    finish();
}

void StatementRecovery::finish()
{
    journalFile_.reset();
    capturedPages_.clear();
    originalFiles_.clear();
    participants_.clear();
    appendOffset_ = 0;
    syncOffset_ = 0;
    recoveryEnd_ = 0;
    recoveryReady_ = false;
    journalDirectoryNeedsSync_ = false;
    failed_ = false;
    active_ = false;
    retirement_ = Retirement::None;
    uuid_clear(statementId_);
}

void StatementRecovery::ensureDurable(JournalCoverage coverage)
{
    requireWritableStatement();
    if (coverage.generation != generation_)
        throw std::logic_error("Stale journal coverage generation");
    ensureDurable(coverage.end);
}
void StatementRecovery::ensureDurable(std::uint64_t requiredEnd)
{
    requireWritableStatement();
    if (requiredEnd > appendOffset_)
        throw std::logic_error("Required journal offset has not been appended");
    if (requiredEnd > syncOffset_ || journalDirectoryNeedsSync_)
    {
        ensureDurable();
    }
}

void StatementRecovery::ensureDurable()
{
    requireWritableStatement();

    try
    {
        if (journalFile_->size() != appendOffset_)
            throw std::runtime_error("Journal size does not match completed appends");
        journalFile_->sync();

        if (journalDirectoryNeedsSync_)
        {
            LinuxDirectory directory{journalPath_.parent_path()};
            directory.sync();
        }
    }
    catch (...)
    {
        failed_ = true;
        throw;
    }
    // Only update this after both required syncs succeed.
    syncOffset_ = appendOffset_;
    journalDirectoryNeedsSync_ = false;
}
