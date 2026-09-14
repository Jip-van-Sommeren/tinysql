#pragma once
#include <fstream>
#include <filesystem>

#include "db_storage.h"

class PageFile
{
public:
    explicit PageFile(const std::filesystem::path &path);

    RawPage readPage(std::uint32_t pageId);
    void writePage(std::uint32_t pageId, const RawPage &page);
    void flush();

private:
    std::fstream file_;
};
