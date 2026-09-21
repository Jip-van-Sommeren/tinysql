#include "db_statement_recovery.h"

StatementRecovery::StatementRecovery(const std::filesystem::path &path,
                                     LinuxFile::OpenMode mode = LinuxFile::OpenMode::CreateNew) : dbRootPath_(path), journalPath_(path / "journal" / "journal.log") {}

void StatementRecovery::begin()
{
    if (journalFile_)
    {
        throw std::logic_error("A statement is already active");
    }

    journalDirectoryNeedsSync_ = true;

    journalFile_.emplace(
        journalPath_,
        LinuxFile::OpenMode::CreateNew);
}

void StatementRecovery::capturePageOnce(const PageBeforeImage &image)
{
    journalFile_->writePage(image);
    capturedPages_[image.relativeFilePath].insert(image.pageId);
}
bool StatementRecovery::isActive() {}
bool StatementRecovery::hasCapturedPage(const std::filesystem::path &path, std::uint32_t pageId) const
{
    const auto file = capturedPages_.find(path);

    return file != capturedPages_.end() &&
           file->second.contains(pageId);
}
void StatementRecovery::rollback()
{
    if (!journalFile_)
    {
        throw std::logic_error("No active journal");
    }
    std::unordered_map<std::filesystem::path, PageFile> openedFiles;

    journalFile_->rewind();

    while (auto image = journalFile_->readNext())
    {
        const auto &relativePath = image->relativeFilePath;
        const auto pageId = image->pageId;
        const RawPage &originalPage = image->originalPage;

        const std::filesystem::path resolvedTablePath =
            dbRootPath_ / image->relativeFilePath;

        auto [it, inserted] = openedFiles.try_emplace(
            image->relativeFilePath, // Map key.
            resolvedTablePath,       // PageFile constructor argument.
            LinuxFile::OpenMode::OpenExisting);

        PageFile &file = it->second;
        file.writePage(image->pageId, image->originalPage);

        // Find/open the PageFile for relativePath.
        // Restore originalPage at pageId.
    }

    for (auto &[path, file] : openedFiles)
    {
        file.sync();
    }

    journalFile_.reset();
    capturedPages_.clear();
}

void StatementRecovery::commit()
{
    if (!std::filesystem::remove(journalPath_))
    {
        throw std::runtime_error("Expected journal file was missing");
    }

    journalFile_.reset();
    capturedPages_.clear();
}
void StatementRecovery::ensureDurable(std::uint32_t requiredEnd) {}

void StatementRecovery::ensureDurable()
{
    if (!journalFile_)
    {
        throw std::logic_error("No active statement");
    }

    if (failed_)
    {
        throw std::runtime_error(
            "Statement has failed; recovery is required");
    }

    try
    {
        // Your existing implementation calls fsync().
        journalFile_->sync();

        if (journalDirectoryNeedsSync_)
        {
            // Open the existing directory; this does not create it.
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
    journalDirectoryNeedsSync_ = false;
}
