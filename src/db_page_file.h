#pragma once
#include <filesystem>

#include "db_linux_file.h"
#include "db_storage.h"

class PageFile
{
public:
    explicit PageFile(
        const std::filesystem::path &path,
        LinuxFile::OpenMode mode = LinuxFile::OpenMode::OpenExisting);

    RawPage readPage(std::uint32_t pageId);
    void writePage(std::uint32_t pageId, const RawPage &page);
    // Compatibility operation: native writes have no application buffer to
    // flush. This does NOT promise durable storage; use sync() for that.
    void flush() noexcept;
    void sync();
    std::filesystem::path getPath() { return file_.getPath(); }
    std::uint64_t size() const
    {
        return file_.size();
    }

    void resize(std::uint64_t sizeInBytes)
    {
        file_.resize(sizeInBytes);
    }

private:
    LinuxFile file_;
};
