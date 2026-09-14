#pragma once

#include "db_storage.h"

#include <filesystem>

struct PageBeforeImage
{
    std::filesystem::path relativeFilePath;
    std::uint32_t pageId;
    RawPage originalPage;
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