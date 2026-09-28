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
    // path_length and  pageId = 2* sizeof(std::uint32_t)
    constexpr std::size_t RecordOverhead = 2 * sizeof(std::uint32_t) + PAGE_SIZE + sizeof(std::uint8_t);
}

std::size_t PageBeforeImage::size() const
{
    return RecordOverhead + encodedPath(relativeFilePath).size();
}

JournalFile::JournalFile(const std::filesystem::path &path, LinuxFile::OpenMode mode)
    : file_(path, mode), appendOffset_(file_.size())
{
}

std::optional<JournalRecord> JournalFile::readRecord(
    std::uint64_t &offset, std::uint64_t fileSize)
{
    constexpr std::size_t TypeSize = sizeof(std::uint8_t);
    constexpr std::size_t PrefixSize = TypeSize + sizeof(std::uint32_t);

    if (offset == fileSize)
    {
        return std::nullopt;
    }
    if (offset > fileSize || fileSize - offset < PrefixSize)
    {
        throw std::runtime_error("Truncated journal record length");
    }
    const auto recordStart = offset;

    // Every record starts with [type:u8][path length:u32].
    std::array<std::byte, PrefixSize> prefixBytes{};
    file_.readExactAt(recordStart, prefixBytes);
    ByteReader prefixReader(prefixBytes);
    const auto journalType = static_cast<JournalRecordType>(
        prefixReader.readUnsigned<std::uint8_t>());
    const auto pathLength = prefixReader.readUnsigned<std::uint32_t>();
    if (pathLength == 0 || pathLength > MaxPathBytes)
    {
        throw std::runtime_error("Invalid journal path length");
    }

    std::size_t recordSize;
    if (journalType == JournalRecordType::FileBeforeImage)
    {
        recordSize = sizeof(std::uint32_t) + sizeof(std::uint8_t) + sizeof(std::uint64_t) + pathLength;
    }
    else if (journalType == JournalRecordType::PageBeforeImage)
    {
        recordSize = RecordOverhead + pathLength;
    }
    else
    {
        throw std::runtime_error("invalid Journal Record Type");
    }

    if (recordSize > fileSize - recordStart)
    {
        throw std::runtime_error("Truncated journal record");
    }

    // At most one bounded record is buffered, even for very large journals.
    std::vector<std::byte> bytes(recordSize);
    std::copy(prefixBytes.begin(), prefixBytes.end(), bytes.begin());
    file_.readExactAt(recordStart + PrefixSize,
                      std::span<std::byte>{bytes}.subspan(PrefixSize));
    ByteReader reader(bytes);
    // The type was decoded above; readString() starts at the path length.
    reader.seek(TypeSize);
    if (journalType == JournalRecordType::FileBeforeImage)
    {
        FileBeforeImage record{};
        record.relativeFilePath = reader.readString();
        (void)encodedPath(record.relativeFilePath);
        record.originalSize = reader.readUnsigned<std::uint64_t>();

        offset = recordStart + recordSize;
        return record;
    }
    // Dont need to check for Other JournalRecordType again since invalid type should throw in the section above
    PageBeforeImage record{};
    record.relativeFilePath = reader.readString();
    (void)encodedPath(record.relativeFilePath);
    record.pageId = reader.readUnsigned<std::uint32_t>();
    reader.readBytes(record.originalPage.data(), record.originalPage.size());

    offset = recordStart + recordSize;
    return record;
}

std::optional<JournalRecord> JournalFile::readNext()
{
    return readRecord(readOffset_, file_.size());
}

void JournalFile::writeFileBeforeImage(const FileBeforeImage &fileBeforeImage)
{
    const std::string path = encodedPath(fileBeforeImage.relativeFilePath);
    ByteWriter writer(sizeof(std::uint32_t) + path.size() + sizeof(std::uint8_t) + sizeof(std::uint64_t));
    writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(JournalRecordType::FileBeforeImage));

    writer.writeString(path);
    writer.writeUnsigned<std::uint64_t>(fileBeforeImage.originalSize);
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
void JournalFile::writePage(const PageBeforeImage &pageBeforeImage)
{
    if (appendFailed_)
    {
        throw std::runtime_error("Journal append previously failed; recovery is required");
    }

    const std::string path = encodedPath(pageBeforeImage.relativeFilePath);
    ByteWriter writer(RecordOverhead + path.size());
    writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(JournalRecordType::PageBeforeImage));
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
