#include "db_journal_file.h"
#include "db_page_file.h"
#include "db_read.h"
#include "db_table.h"
#include "db_write.h"

#include <array>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{
    using Mode = LinuxFile::OpenMode;

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <typename Error, typename Function>
    void requireThrows(Function function, const std::string &message = "")
    {
        try
        {
            function();
        }
        catch (const Error &error)
        {
            require(std::string{error.what()}.find(message) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected exception: " + message);
    }

    struct TestDirectory
    {
        std::filesystem::path path;
        TestDirectory()
        {
            auto pattern = (std::filesystem::temp_directory_path() / "db-journal-test-XXXXXX").string();
            const char *created = ::mkdtemp(pattern.data());
            if (!created)
                throw std::runtime_error("Cannot create test directory");
            path = created;
        }
        ~TestDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    PageBeforeImage makeRecord(const std::string &path, std::uint32_t pageId, std::byte value)
    {
        PageBeforeImage result{.relativeFilePath = path, .pageId = pageId, .originalPage = {}};
        result.originalPage.bytes.fill(value);
        return result;
    }

    void requireRecord(const JournalRecord &record, const PageBeforeImage &expected)
    {
        require(std::holds_alternative<PageBeforeImage>(record), "Expected page record");
        const auto &actual = std::get<PageBeforeImage>(record);
        require(actual.relativeFilePath == expected.relativeFilePath &&
                    actual.pageId == expected.pageId &&
                    actual.originalPage.bytes == expected.originalPage.bytes,
                "Journal record changed");
    }

    void requireRecord(const JournalRecord &record, const FileBeforeImage &expected)
    {
        require(std::holds_alternative<FileBeforeImage>(record), "Expected file record");
        const auto &actual = std::get<FileBeforeImage>(record);
        require(actual.relativeFilePath == expected.relativeFilePath &&
                    actual.originalSize == expected.originalSize && actual.newFile == expected.newFile,
                "Journal file metadata changed");
    }

    void testRoundTrip(const std::filesystem::path &directory)
    {
        const auto path = directory / "undo.journal";
        const auto first = makeRecord("tables/one.table", 1, std::byte{0xa1});
        const auto second = makeRecord("tables/two.table", 1, std::byte{0xb2});
        const FileBeforeImage metadata{"tables/one.table", (1ULL << 40) + PAGE_SIZE};
        {
            JournalFile journal(path);
            require(!journal.readNext(), "Empty journal read failed");
            journal.writePage(first);
            journal.writeFileBeforeImage(metadata);
            journal.writePage(second);
            journal.sync();
            requireRecord(journal.readNext().value(), first);
            requireRecord(journal.readNext().value(), metadata);
            requireRecord(journal.readNext().value(), second);
            require(!journal.readNext() && !journal.readNext(), "Journal EOF is not stable");
            journal.rewind();
            requireRecord(journal.readNext().value(), first);
        }
        const auto originalSize = std::filesystem::file_size(path);
        requireThrows<std::system_error>([&] { JournalFile duplicate(path); }, "open");
        require(std::filesystem::file_size(path) == originalSize, "New journal overwrote old journal");
        {
            JournalFile reopened(path, Mode::OpenExisting);
            reopened.writePage(first); // Preserve order and duplicates, not just a map.
            requireRecord(reopened.readNext().value(), first);
            requireRecord(reopened.readNext().value(), metadata);
            requireRecord(reopened.readNext().value(), second);
            requireRecord(reopened.readNext().value(), first);
            require(!reopened.readNext(), "Reopened journal did not end after appended record");
            require(std::filesystem::file_size(path) == originalSize + first.size(),
                    "Encoded record size mismatch");
        }
        JournalFile reader(path, Mode::ReadOnly);
        requireRecord(reader.readNext().value(), first);
        requireRecord(reader.readNext().value(), metadata);
        requireRecord(reader.readNext().value(), second);
        requireRecord(reader.readNext().value(), first);
        require(!reader.readNext(), "Read-only journal reopen failed");
        requireThrows<std::system_error>([&] { reader.writePage(first); }, "pwrite");

        // Check the tagged [type][length][path][ID][RawPage] format.
        LinuxFile raw(path, Mode::ReadOnly);
        std::vector<std::byte> bytes(first.size());
        raw.readExactAt(0, bytes);
        ByteReader decoder(bytes);
        require(decoder.readUnsigned<std::uint8_t>() ==
                    static_cast<std::uint8_t>(JournalRecordType::PageBeforeImage),
                "Record type encoding changed");
        require(decoder.readString() == first.relativeFilePath.generic_string(), "Path encoding changed");
        require(decoder.readUnsigned<std::uint32_t>() == first.pageId, "ID encoding changed");
        RawPage page{};
        decoder.readBytes(page.data(), page.size());
        require(page.bytes == first.originalPage.bytes, "Before-image encoding changed");
    }

    void testMalformedRecords(const std::filesystem::path &directory)
    {
        {
            JournalFile journal(directory / "new-file");
            const FileBeforeImage metadata{"tables/new.table", 0, true};
            require(journal.writeFileBeforeImage(metadata) == journal.size(),
                    "Metadata append did not return its end offset");
            requireRecord(journal.readNext().value(), metadata);
            require(!journal.readNext(), "New-file metadata did not end at EOF");
        }
        const auto path = directory / "truncated";
        {
            LinuxFile raw(path, Mode::CreateNew);
            const std::array<std::byte, 3> shortPrefix{};
            raw.writeAllAt(0, shortPrefix);
        }
        JournalFile shortPrefix(path, Mode::ReadOnly);
        requireThrows<std::runtime_error>([&] { shortPrefix.readNext(); }, "Truncated");
        requireThrows<std::runtime_error>([&] { shortPrefix.readNext(); }, "Truncated");

        const auto oversized = directory / "oversized";
        {
            LinuxFile raw(oversized, Mode::CreateNew);
            ByteWriter writer(5);
            writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(JournalRecordType::PageBeforeImage));
            writer.writeUnsigned<std::uint32_t>(std::numeric_limits<std::uint32_t>::max());
            raw.writeAllAt(0, writer.bytes());
        }
        JournalFile oversizedReader(oversized, Mode::ReadOnly);
        requireThrows<std::runtime_error>([&] { oversizedReader.readNext(); }, "path length");

        const auto body = directory / "short-body";
        {
            LinuxFile raw(body, Mode::CreateNew);
            ByteWriter writer(5);
            writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(JournalRecordType::PageBeforeImage));
            writer.writeUnsigned<std::uint32_t>(1);
            raw.writeAllAt(0, writer.bytes());
        }
        JournalFile bodyReader(body, Mode::ReadOnly);
        requireThrows<std::runtime_error>([&] { bodyReader.readNext(); }, "Truncated");

        const auto invalid = directory / "invalid-path";
        JournalFile invalidWriter(invalid);
        for (const auto &name : {std::string{}, std::string{"/absolute"}, std::string{"../outside"},
                                 std::string{"x\0y", 3}, std::string(JournalFile::MaxPathBytes + 1, 'x')})
        {
            const auto record = makeRecord(name, 0, std::byte{0});
            requireThrows<std::runtime_error>([&] { invalidWriter.writePage(record); });
        }
        require(std::filesystem::file_size(invalid) == 0, "Invalid record partially written");
        {
            LinuxFile raw(invalid);
            ByteWriter writer(1 + 4 + 2 + 4 + PAGE_SIZE);
            writer.writeUnsigned<std::uint8_t>(static_cast<std::uint8_t>(JournalRecordType::PageBeforeImage));
            writer.writeString("..");
            writer.writeUnsigned<std::uint32_t>(0);
            raw.writeAllAt(0, writer.bytes());
        }
        JournalFile invalidReader(invalid, Mode::ReadOnly);
        requireThrows<std::runtime_error>([&] { invalidReader.readNext(); }, "path");
    }

    void testLargeJournalStreaming(const std::filesystem::path &directory)
    {
        const auto path = directory / "large";
        const auto record = makeRecord("tables/one.table", 1, std::byte{0x71});
        {
            JournalFile writer(path);
            writer.writePage(record);
        }
        LinuxFile raw(path);
        raw.resize(1ULL << 30); // Sparse, not a gigabyte allocation/write.
        JournalFile reader(path, Mode::ReadOnly);
        requireRecord(reader.readNext().value(), record);
        requireThrows<std::runtime_error>([&] { reader.readNext(); }, "path length");
    }

    void testPageFile(const std::filesystem::path &directory)
    {
        const auto path = directory / "pages";
        requireThrows<std::system_error>([&] { PageFile missing(path); }, "open");
        require(!std::filesystem::exists(path), "PageFile reopen created missing file");
        PageFile pages(path, Mode::CreateNew);
        RawPage expected{};
        expected.bytes.fill(std::byte{0x9a});
        pages.writePage(3, expected);
        PageFile reopened(path);
        require(reopened.readPage(3).bytes == expected.bytes, "Unflushed page write was not visible");
        require(readPageFromFile(path, 3).bytes == expected.bytes, "Standalone reader changed behavior");
        requireThrows<std::runtime_error>([&] { pages.readPage(4); }, "EOF");
        requireThrows<std::system_error>([&] { PageFile duplicate(path, Mode::CreateNew); }, "open");
        pages.sync();

        const auto missingTable = directory / "missing.table";
        StatementRecovery recovery{directory};
        requireThrows<std::system_error>([&] { Table::open(directory, "missing.table", recovery); }, "open");
        require(!std::filesystem::exists(missingTable), "Table::open created a missing table");
    }
}

int main()
{
    TestDirectory directory;
    testRoundTrip(directory.path);
    testMalformedRecords(directory.path);
    testLargeJournalStreaming(directory.path);
    testPageFile(directory.path);
}
