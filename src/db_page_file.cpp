#include "db_page_file.h"
#include <stdexcept>

namespace
{
    std::fstream openOrCreateFile(const std::filesystem::path &path)
    {
        if (!std::filesystem::exists(path))
        {
            std::ofstream file{path, std::ios::binary};
            if (!file)
            {
                throw std::runtime_error(
                    "Failed to create file: " + path.string());
            }
        }

        std::fstream file{
            path,
            std::ios::in | std::ios::out | std::ios::binary};

        if (!file)
        {
            throw std::runtime_error(
                "Failed to open file: " + path.string());
        }
        return file;
    }

}

PageFile::PageFile(const std::filesystem::path &path)
    : file_(openOrCreateFile(path))
{
    file_.exceptions(std::ios::failbit | std::ios::badbit);
}

RawPage PageFile::readPage(std::uint32_t pageId)
{
    RawPage page{};

    file_.seekg(static_cast<std::streamoff>(pageId) * PAGE_SIZE);
    file_.read(
        reinterpret_cast<char *>(page.data()),
        static_cast<std::streamsize>(page.size()));

    return page;
}
void PageFile::writePage(std::uint32_t pageId, const RawPage &page)
{

    file_.seekp(
        static_cast<std::streamoff>(pageId) * PAGE_SIZE,
        std::ios::beg);

    file_.write(
        reinterpret_cast<const char *>(page.data()),
        static_cast<std::streamsize>(page.size()));
}
void PageFile::flush()
{
    file_.flush();
    if (!file_)
    {
        throw std::runtime_error("Failed to flush file");
    }
}