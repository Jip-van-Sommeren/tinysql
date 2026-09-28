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
    active_ = true;

    journalFile_.emplace(
        journalPath_,
        LinuxFile::OpenMode::CreateNew);
}

void StatementRecovery::capturePageOnce(const PageBeforeImage &image)
{
    journalFile_->writePage(image);
    capturedPages_[image.relativeFilePath].insert(image.pageId);
}
bool StatementRecovery::isActive() { return active_; }
bool StatementRecovery::hasCapturedPage(const std::filesystem::path &path, std::uint32_t pageId) const
{
    const auto file = capturedPages_.find(path);

    return file != capturedPages_.end() &&
           file->second.contains(pageId);
}

void StatementRecovery::captureFileOnce(const std::filesystem::path &relativePath)
{
    if (!originalFileSizes_.contains(relativePath))
    {
        const auto originalSize = PageFile(relativePath).size();

        // TODO
        journalFile_->writeFileBeforeImage(FileBeforeImage{
            .relativeFilePath = relativePath,
            .originalSize = originalSize,
        });

        originalFileSizes_.emplace(relativePath, originalSize);
    }
}

void StatementRecovery::rollback()
{
    if (!journalFile_)
    {
        throw std::logic_error("No active journal");
    }
    std::unordered_map<std::filesystem::path, PageFile> openedFiles;

    journalFile_->rewind();
    std::unordered_map<std::filesystem::path, std::uint64_t>
        rollbackFileSizes;
    while (auto image = journalFile_->readNext())
    {
        std::visit(
            [&](const auto &value)
            {
                using T = std::decay_t<decltype(value)>;

                if constexpr (std::is_same_v<T, PageBeforeImage>)
                {
                    const auto &relativePath = value.relativeFilePath;
                    const auto pageId = value.pageId;
                    const RawPage &originalPage = value.originalPage;

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
                    auto [it, inserted] = rollbackFileSizes.try_emplace(
                        value.relativeFilePath,
                        value.originalSize);

                    if (!inserted && it->second != value.originalSize)
                    {
                        throw std::runtime_error(
                            "Conflicting original file sizes in journal");
                    }
                }
            },
            image.value());
    }
    try
    {

        for (const auto &[relativePath, originalSize] : rollbackFileSizes)
        {
            auto [it, inserted] = openedFiles.try_emplace(
                relativePath,
                dbRootPath_ / relativePath,
                LinuxFile::OpenMode::OpenExisting);

            it->second.resize(originalSize);
            it->second.sync();
        }
        LinuxDirectory directory{journalPath_.parent_path()};
        directory.sync();
    }
    catch (...)
    {
        throw;
    }

    if (!std::filesystem::remove(journalPath_))
    {
        throw std::runtime_error("Expected journal file was missing");
    }

    journalFile_.reset();
    capturedPages_.clear();
    originalFileSizes_.clear();
    failed_ = false;
}

void StatementRecovery::commit()
{
    if (!std::filesystem::remove(journalPath_))
    {
        throw std::runtime_error("Expected journal file was missing");
    }

    journalFile_.reset();
    capturedPages_.clear();
    originalFileSizes_.clear();
    failed_ = false;
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
