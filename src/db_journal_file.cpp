#include "db_journal_file.h"

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

JournalFile::JournalFile(const std::filesystem::path &path)
    : file_(openOrCreateFile(path))
{
    file_.exceptions(std::ios::failbit | std::ios::badbit);
}

void JournalFile::writePage(PageBeforeImage pageBeforeImage)
{

    file_.seekp(
        offset_,
        std::ios::beg);

    file_.write(reinterpret_cast<const char *>(pageBeforeImage.pageId),
                static_cast<std::streamsize>(sizeof(pageBeforeImage.pageId)));
    file_.write(
        reinterpret_cast<const char *>(pageBeforeImage.originalPage.data()),
        static_cast<std::streamsize>(pageBeforeImage.originalPage.size()));

    offset_ += page.size() + sizeof(pageId);
}