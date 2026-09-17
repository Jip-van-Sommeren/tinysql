#include "db_journal_file.h"

#include "db_read.h"
#include "db_write.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    std::string encodedPath(const std::filesystem::path &path)
    {
        const std::string value = path.generic_string();
        if (value.empty() || value.size() > JournalFile::MaxPathBytes ||
            value.find('\0') != std::string::npos || path.has_root_path())
        {
            throw std::runtime_error("Invalid journal relative file path");
        }
        for (const auto &component : path)
        {
            if (component == "..")
            {
                throw std::runtime_error("Journal path must not contain '..'");
            }
        }
        return value;
    }

    constexpr std::size_t RecordOverhead = 2 * sizeof(std::uint32_t) + PAGE_SIZE;
}

std::size_t PageBeforeImage::size() const
{
    return RecordOverhead + encodedPath(relativeFilePath).size();
}

JournalFile::JournalFile(const std::filesystem::path &path, LinuxFile::OpenMode mode)
    : file_(path, mode), appendOffset_(file_.size())
{
}

std::optional<PageBeforeImage> JournalFile::readRecord(
    std::uint64_t &offset, std::uint64_t fileSize)
{
    if (offset == fileSize)
    {
        return std::nullopt;
    }
    if (offset > fileSize || fileSize - offset < sizeof(std::uint32_t))
    {
        throw std::runtime_error("Truncated journal record length");
    }

    std::array<std::byte, sizeof(std::uint32_t)> lengthBytes{};
    file_.readExactAt(offset, lengthBytes);
    ByteReader lengthReader(lengthBytes);
    const auto pathLength = lengthReader.readUnsigned<std::uint32_t>();
    if (pathLength == 0 || pathLength > MaxPathBytes)
    {
        throw std::runtime_error("Invalid journal path length");
    }

    const std::size_t recordSize = RecordOverhead + pathLength;
    if (recordSize > fileSize - offset)
    {
        throw std::runtime_error("Truncated journal record");
    }

    // At most one bounded record is buffered, even for very large journals.
    std::vector<std::byte> bytes(recordSize);
    std::copy(lengthBytes.begin(), lengthBytes.end(), bytes.begin());
    file_.readExactAt(offset + lengthBytes.size(),
                      std::span<std::byte>{bytes}.subspan(lengthBytes.size()));
    ByteReader reader(bytes);
    PageBeforeImage record{};
    record.relativeFilePath = reader.readString();
    (void)encodedPath(record.relativeFilePath);
    record.pageId = reader.readUnsigned<std::uint32_t>();
    reader.readBytes(record.originalPage.data(), record.originalPage.size());

    offset += recordSize;
    return record;
}

std::optional<PageBeforeImage> JournalFile::readNext()
{
    return readRecord(readOffset_, file_.size());
}

std::vector<PageBeforeImage> JournalFile::readPages()
{
    std::vector<PageBeforeImage> pages;
    const auto fileSize = file_.size();
    std::uint64_t offset = 0;
    while (auto record = readRecord(offset, fileSize))
    {
        pages.push_back(std::move(*record));
    }
    return pages;
}

void JournalFile::writePage(const PageBeforeImage &pageBeforeImage)
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }

    const std::string path = encodedPath(pageBeforeImage.relativeFilePath);
    ByteWriter writer(RecordOverhead + path.size());
    writer.writeString(path);
    writer.writeUnsigned<std::uint32_t>(pageBeforeImage.pageId);
    writer.writeBytes(pageBeforeImage.originalPage.data(), pageBeforeImage.originalPage.size());

    if (writer.size() > std::numeric_limits<std::uint64_t>::max() - appendOffset_)
    {
        throw std::out_of_range("Journal append offset overflow");
    }
    try
    {
        file_.writeAllAt(appendOffset_, writer.bytes());
    }
    catch (...)
    {
        // A partial record may have reached disk. Never append past it on this
        // object or report it as a successfully synchronized record stream.
        appendFailed_ = true;
        throw;
    }
    appendOffset_ += writer.size();
}

void JournalFile::sync()
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }
    file_.sync();
}
