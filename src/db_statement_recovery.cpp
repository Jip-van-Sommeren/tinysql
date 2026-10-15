#include "db_statement_recovery.h"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <set>
#include <zlib.h>
#include <uuid/uuid.h>

// namespace
// {

//     std::uint32_t checksum(std::span<const std::byte> bytes)
//     {
//         return static_cast<std::uint32_t>(
//             crc32_z(0,
//                     reinterpret_cast<const Bytef *>(bytes.data()),
//                     bytes.size()));
//     }
//     // path_length and  pageId = 2* sizeof(std::uint32_t)
// }

StatementRecovery::StatementRecovery(
    const LinuxDirectory &root,
    const uuid_t &dbId)
    : dbRootDir_(root, "."),
      journalDir_(dbRootDir_, "journal"),
      tableDir_(dbRootDir_, "tables")
{
    uuid_copy(dbId_, dbId);
}

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

void StatementRecovery::validateJournalForRecovery(std::uint64_t end)
{

    if (!journalFile_)
    {
        throw std::logic_error("No journal open for recovery");
    }

    journalFile_->rewind();

    const auto header = journalFile_->readJournalHeader();
    // uuid_copy(statementId_, header.statementId);

    if (uuid_compare(header.dbId, dbId_) != 0)
    {
        throw std::runtime_error("Journal belongs to another database");
    }

    if (uuid_compare(header.statementId, statementId_) != 0)
    {
        throw std::runtime_error("Journal belongs to another database");
    }

    std::unordered_map<std::filesystem::path, FileBeforeImage> rollbackFiles;
    std::set<std::pair<std::filesystem::path, std::uint32_t>> seenPages;
    while (auto image = journalFile_->readNext(end))
    {
        auto payload = image.value().record;
        std::visit(
            [&](const auto &value)
            {
                using T = std::decay_t<decltype(value)>;

                if constexpr (std::is_same_v<T, PageBeforeImage>)
                {
                    if (!seenPages.emplace(value.relativeFilePath, value.pageId).second)
                    {
                        throw std::runtime_error("Duplicate page before-image in journal");
                    }

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
            payload);
    }
    journalFile_->rewind(JournalDataOffset);
    return;
}

void StatementRecovery::initializeForRecover()
{
    if (journalFile_ || active_ || failed_)
        throw std::logic_error("A statement is already active");
    failed_ = true; // No normal access until replay and cleanup finish.
    journalFile_.emplace(
        journalDir_, "journal.log", dbId_, statementId_,
        LinuxFile::OpenMode::OpenExisting);

    const auto end = journalFile_->validateJournalHeader(statementId_);
    if (end < JournalDataOffset || end > journalFile_->size())
    {
        throw std::runtime_error("invalid Journal endoffset");
    }
    recoveryEnd_ = end;
    recoveryReady_ = true;

    journalFile_->sync();

    // LinuxDirectory directory{journalDir_.getPath().parent_path()};
    journalDir_.sync();

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
    uuid_generate_random(statementId_);

    journalFile_.emplace(
        journalDir_, "journal.log", dbId_, statementId_,
        LinuxFile::OpenMode::CreateNew);

    JournalHeader header{};
    header.magic = JournalMagic;
    header.version = 1;
    header.headerLength = JournalHeaderBytes;
    uuid_copy(header.dbId, dbId_);
    uuid_copy(header.statementId, statementId_);
    header.pageSize = PAGE_SIZE;
    header.checksum = 0;

    JournalBoundary journalBoundary{.generation = 0, .endOffset = JournalDataOffset, .checksum = 0};

    journalFile_->writeJournalHeader(header);
    journalFile_->writeJournalBoundary(journalBoundary, 0);
    journalFile_->writeJournalBoundary(journalBoundary, 1);

    appendOffset_ = JournalDataOffset;
    recoveryEnd_ = JournalDataOffset;
    ++generation_;
    recoveryReady_ = true;

    journalDirectoryNeedsSync_ = true;
    active_ = true;
    ensureDurable();
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

        // const auto resolvedPath = tableDir_.getPath() / relativePath;
        if (newFile && tableDir_.entryExists(relativePath))
        {
            throw std::runtime_error("Table already exists: " + relativePath.string());
        }
        if (const auto existing = originalFiles_.find(relativePath); existing != originalFiles_.end())
        {
            if (newFile && !existing->second.beforeImage.newFile)
                throw std::logic_error("Cannot recreate a file that existed before the statement");
            return;
        }

        // A new file does not exist yet; opening it here would violate write-ahead ordering.
        const auto originalSize = newFile ? std::uint64_t{0} : PageFile(tableDir_, relativePath).size();
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

    if (retirement_ == Retirement::Rollback)
    {
        // LinuxDirectory directory{journalPath_.parent_path()};
        journalDir_.sync();
        finish();
        return;
    }
    if (cacheEpoch_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Cache epoch exhausted");
    std::unordered_map<std::filesystem::path, PageFile> openedFiles;
    std::unordered_map<std::filesystem::path, FileBeforeImage> rollbackFiles;

    validateJournalForRecovery(recoveryEnd_);
    // journalFile_->rewind();

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

                    auto [it, inserted] = openedFiles.try_emplace(
                        value.relativeFilePath,
                        tableDir_, value.relativeFilePath.filename(),
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

    bool tableDirChanged{false};

    for (const auto &[relativePath, metadata] : rollbackFiles)
    {
        if (metadata.newFile)
        {
            // Already absent is success: creation or a previous rollback may have failed.
            // LinuxDirectory dir{relativePath.parent_path()};
            tableDir_.deleteFile(relativePath.filename());
            // (void)std::filesystem::remove(dbRootPath_ / relativePath);

            tableDirChanged = true;

            continue;
        }
        auto [it, inserted] = openedFiles.try_emplace(
            relativePath,
            tableDir_, relativePath.filename(),
            LinuxFile::OpenMode::OpenExisting);

        it->second.resize(metadata.originalSize);
        it->second.sync();
    }
    if (tableDirChanged)
        tableDir_.sync();
    journalDir_.deleteFile("journal.log");

    retirement_ = Retirement::Rollback;
    ++cacheEpoch_;
    // LinuxDirectory directory{journalPath_.parent_path()};
    journalDir_.sync();

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
        bool tableDirChanged{false};
        for (const auto &[relativePath, metadata] : originalFiles_)
        {
            if (metadata.beforeImage.newFile)
            {
                tableDirChanged = true;
                break;
            }
        }
        if (tableDirChanged)
            tableDir_.sync();
        journalDir_.deleteFile("journal.log");

        retirement_ = Retirement::Commit;
        journalDir_.sync();
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
    const auto nextSlot = 1 - activeBoundarySlot_;

    try
    {
        if (journalFile_->size() != appendOffset_)
            throw std::runtime_error("Journal size does not match completed appends");
        journalFile_->sync();
        journalFile_->writeJournalBoundary(
            JournalBoundary{
                .generation = boundaryGeneration_ + 1,
                .endOffset = appendOffset_,
                .checksum = 0},
            nextSlot);

        journalFile_->sync();

        if (journalDirectoryNeedsSync_)
        {
            journalDir_.sync();
        }
    }
    catch (...)
    {
        failed_ = true;
        throw;
    }
    // Only update this after both required syncs succeed.
    activeBoundarySlot_ = nextSlot;
    ++boundaryGeneration_;
    syncOffset_ = appendOffset_;
    journalDirectoryNeedsSync_ = false;
}
