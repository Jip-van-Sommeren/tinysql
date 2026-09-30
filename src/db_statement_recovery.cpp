#include "db_statement_recovery.h"
#include <stdexcept>

StatementRecovery::StatementRecovery(const std::filesystem::path &path)
    : dbRootPath_(path), journalPath_(path / "journal" / "journal.log") {}

void StatementRecovery::requireWritableStatement() const
{
    if (!active_ || !journalFile_)
    {
        throw std::logic_error("No active statement");
    }
    if (failed_)
    {
        throw std::runtime_error("Statement has failed; recovery is required");
    }
}

void StatementRecovery::begin()
{
    if (journalFile_ || active_ || failed_)
    {
        throw std::logic_error("A statement is already active");
    }

    journalFile_.emplace(
        journalPath_,
        LinuxFile::OpenMode::CreateNew);
    appendOffset_ = 0;
    syncOffset_ = 0;
    journalDirectoryNeedsSync_ = true;
    active_ = true;
}

void StatementRecovery::capturePageOnce(const PageBeforeImage &image)
{
    requireWritableStatement();
    if (hasCapturedPage(image.relativeFilePath, image.pageId))
        return;
    if (std::uint64_t{image.pageId} * PAGE_SIZE >= getOriginalFileSize(image.relativeFilePath))
        throw std::logic_error("Page before-image is outside the original file");
    try
    {
        appendOffset_ = journalFile_->writePage(image);
        capturedPages_[image.relativeFilePath].insert(image.pageId);
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
    const auto resolvedPath = dbRootPath_ / relativePath;
    if (newFile && std::filesystem::exists(std::filesystem::symlink_status(resolvedPath)))
    {
        throw std::runtime_error("Table already exists: " + resolvedPath.string());
    }
    if (const auto existing = originalFiles_.find(relativePath); existing != originalFiles_.end())
    {
        if (newFile && !existing->second.newFile)
            throw std::logic_error("Cannot recreate a file that existed before the statement");
        return;
    }

    // A new file does not exist yet; opening it here would violate write-ahead ordering.
    const auto originalSize = newFile ? std::uint64_t{0} : PageFile(resolvedPath).size();
    if (originalSize % PAGE_SIZE != 0)
        throw std::runtime_error("Invalid original table file size");

    const FileBeforeImage image{
        .relativeFilePath = relativePath, .originalSize = originalSize, .newFile = newFile};
    try
    {
        appendOffset_ = journalFile_->writeFileBeforeImage(image);
        originalFiles_.emplace(relativePath, image);
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
    return file != originalFiles_.end() && file->second.newFile;
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

    return originalFiles_.at(relativePath).originalSize;
}

void StatementRecovery::rollback()
{
    if (!journalFile_)
    {
        throw std::logic_error("No active journal");
    }
    failed_ = true;
    std::unordered_map<std::filesystem::path, PageFile> openedFiles;
    std::unordered_map<std::filesystem::path, FileBeforeImage> rollbackFiles;

    journalFile_->rewind();

    while (auto image = journalFile_->readNext())
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
            image.value());
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
    LinuxDirectory directory{journalPath_.parent_path()};
    directory.sync();

    finish();
}

void StatementRecovery::commit()
{
    requireWritableStatement();
    try
    {
        // Table mutation methods synchronize file contents before commit. Persist
        // new names too, including callers using StorageEngine directly.
        std::unordered_set<std::filesystem::path> changedDirectories;
        for (const auto &[relativePath, metadata] : originalFiles_)
        {
            if (metadata.newFile)
                changedDirectories.insert((dbRootPath_ / relativePath).parent_path());
        }
        for (const auto &path : changedDirectories)
        {
            LinuxDirectory directory{path};
            directory.sync();
        }
        if (!std::filesystem::remove(journalPath_))
            throw std::runtime_error("Expected journal file was missing");
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
    appendOffset_ = 0;
    syncOffset_ = 0;
    journalDirectoryNeedsSync_ = false;
    failed_ = false;
    active_ = false;
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
