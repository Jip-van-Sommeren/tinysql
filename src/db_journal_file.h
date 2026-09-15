#pragma once

#include "db_storage.h"
#include "db_write.h"

#include <filesystem>
#include <unordered_map>

struct PageBeforeImage
{
    std::filesystem::path relativeFilePath;
    std::uint32_t pageId;
    RawPage originalPage;

    std::uint32_t size() const
    {
        return originalPage.size() + 
        sizeof(pageId) + 
        sizeof(std::uint32_t) + //str length also gets written
        relativeFilePath.string().size();
    }
};

// struct JournalFile
// {
//     JournalFileHeader header;
//     RawPage page;
//     std::uint32_t offset;
// };

class JournalFile
{
public:
    explicit JournalFile(const std::filesystem::path &path);

    RawPage readPage(std::uint32_t pageId);
    void writePage(PageBeforeImage pageBeforeImage);
    void flush();

private:
    std::fstream file_;
    std::streamoff offset_{0};
};