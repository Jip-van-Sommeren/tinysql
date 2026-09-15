#include "db_journal_file.h"
#include "db_read.h"
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

std::unordered_map<std::uint32_t, RawPage> JournalFile::readPages()
{
    std::unordered_map<std::uint32_t, RawPage> pages;
    RawPage rawPage{};

    BytesDecoder bytesDecoder(rawPage);
    while (!EOF){
        std::string path = bytesDecoder.decodeString();
        std::uint32_t pageId = bytesDecoder.decodeUnsignedAt<std::uint32_t>();
        RawPage rawPage = read(offset);

        pages.insert({pageId, rawPage});
    }
    return pages;
}

void JournalFile::writePage(PageBeforeImage pageBeforeImage)
{
    ByteWriter byteWriter( pageBeforeImage.size());

    byteWriter.writeString(pageBeforeImage.relativeFilePath.string());
    byteWriter.writeUnsigned<std::uint32_t>(pageBeforeImage.pageId);
    byteWriter.writeBytes(pageBeforeImage.originalPage.data(), pageBeforeImage.originalPage.size());

    file_.seekp(
        offset_,
        std::ios::beg);

    file_.write(reinterpret_cast<const char *>(byteWriter.bytes().data()),
                static_cast<std::streamsize>(byteWriter.size()));

    offset_ += pageBeforeImage.size();
}