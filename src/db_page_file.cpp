#include "db_page_file.h"

PageFile::PageFile(const std::filesystem::path &path, LinuxFile::OpenMode mode)
    : file_(path, mode)
{
}

RawPage PageFile::readPage(std::uint32_t pageId)
{
    RawPage page{};
    file_.readExactAt(std::uint64_t{pageId} * PAGE_SIZE, page.bytes);
    return page;
}

void PageFile::writePage(std::uint32_t pageId, const RawPage &page)
{
    file_.writeAllAt(std::uint64_t{pageId} * PAGE_SIZE, page.bytes);
}

void PageFile::flush() noexcept
{
    // LinuxFile has no userspace write buffer.
}

void PageFile::sync()
{
    file_.sync();
}
