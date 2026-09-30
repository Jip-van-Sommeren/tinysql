#include "db_journal_file.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using Bytes = std::vector<std::byte>;
    using Mode = LinuxFile::OpenMode;

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <typename Function>
    void requireError(Function function, const std::string &message)
    {
        try
        {
            function();
        }
        catch (const std::runtime_error &error)
        {
            require(std::string{error.what()}.find(message) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected error: " + message);
    }

    struct TestDirectory
    {
        std::filesystem::path path;

        TestDirectory()
        {
            auto pattern = (std::filesystem::temp_directory_path() /
                            "db-journal-reader-XXXXXX").string();
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

    template <typename T>
    void appendUnsigned(Bytes &bytes, T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xffu));
    }

    // Encode fixtures independently so a matching writer/reader offset bug
    // cannot hide an incorrect interpretation of the on-disk format.
    Bytes prefix(JournalRecordType type, const std::string &path)
    {
        Bytes bytes;
        appendUnsigned(bytes, static_cast<std::uint8_t>(type));
        appendUnsigned(bytes, static_cast<std::uint32_t>(path.size()));
        for (unsigned char character : path)
            bytes.push_back(static_cast<std::byte>(character));
        return bytes;
    }

    Bytes encode(const FileBeforeImage &record)
    {
        auto bytes = prefix(JournalRecordType::FileBeforeImage,
                            record.relativeFilePath.generic_string());
        appendUnsigned(bytes, record.originalSize);
        appendUnsigned(bytes, static_cast<std::uint8_t>(record.newFile));
        return bytes;
    }

    Bytes encode(const PageBeforeImage &record)
    {
        auto bytes = prefix(JournalRecordType::PageBeforeImage,
                            record.relativeFilePath.generic_string());
        appendUnsigned(bytes, record.pageId);
        bytes.insert(bytes.end(), record.originalPage.bytes.begin(),
                     record.originalPage.bytes.end());
        return bytes;
    }

    void requireRecord(const JournalRecord &actual, const FileBeforeImage &expected)
    {
        const auto *record = std::get_if<FileBeforeImage>(&actual);
        require(record && record->relativeFilePath == expected.relativeFilePath &&
                    record->originalSize == expected.originalSize &&
                    record->newFile == expected.newFile,
                "File before-image changed");
    }

    void requireRecord(const JournalRecord &actual, const PageBeforeImage &expected)
    {
        const auto *record = std::get_if<PageBeforeImage>(&actual);
        require(record && record->relativeFilePath == expected.relativeFilePath &&
                    record->pageId == expected.pageId &&
                    record->originalPage.bytes == expected.originalPage.bytes,
                "Page before-image changed");
    }

    void testRecordBoundaries(const std::filesystem::path &directory)
    {
        const FileBeforeImage file{"tables/one.table", (1ULL << 40) + PAGE_SIZE};
        PageBeforeImage page{"tables/one.table", 17, {}};
        for (std::size_t i = 0; i < page.originalPage.size(); ++i)
            page.originalPage.bytes[i] = static_cast<std::byte>(i % 256);
        const FileBeforeImage otherFile{"tables/other.table", 0, true};
        PageBeforeImage otherPage{"tables/other.table", 17, {}};
        otherPage.originalPage.bytes.fill(std::byte{0xa5});

        const auto path = directory / "records";
        LinuxFile raw(path, Mode::CreateNew);
        JournalFile reader(path, Mode::ReadOnly);
        require(!reader.readNext(), "Empty journal should return EOF");

        // Each type must be readable when it is the only record, with no
        // trailing byte required. Appending after EOF must also work.
        raw.writeAllAt(0, encode(file));
        requireRecord(reader.readNext().value(), file);
        require(!reader.readNext(), "File record did not end exactly at EOF");
        raw.resize(0);
        reader.rewind();
        raw.writeAllAt(0, encode(page));
        requireRecord(reader.readNext().value(), page);
        require(!reader.readNext(), "Page record did not end exactly at EOF");

        raw.resize(0);
        std::uint64_t offset = 0;
        for (const auto &bytes : {encode(file), encode(page), encode(otherFile), encode(otherPage)})
        {
            raw.writeAllAt(offset, bytes);
            offset += bytes.size();
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            reader.rewind();
            requireRecord(reader.readNext().value(), file);
            requireRecord(reader.readNext().value(), page);
            requireRecord(reader.readNext().value(), otherFile);
            requireRecord(reader.readNext().value(), otherPage);
            require(!reader.readNext() && !reader.readNext(), "EOF is not stable");
        }
    }

    void testTruncation(const std::filesystem::path &directory)
    {
        const FileBeforeImage file{"tables/one.table", 2 * PAGE_SIZE};
        const PageBeforeImage page{"tables/one.table", 1, {}};
        const auto path = directory / "truncated";
        LinuxFile raw(path, Mode::CreateNew);
        const auto fileBytes = encode(file);

        // Includes all partial prefixes (one type byte plus four length bytes).
        for (std::size_t size = 1; size < fileBytes.size(); ++size)
        {
            raw.resize(0);
            raw.writeAllAt(0, std::span<const std::byte>{fileBytes}.first(size));
            JournalFile reader(path, Mode::ReadOnly);
            requireError([&] { reader.readNext(); }, "Truncated");
            requireError([&] { reader.readNext(); }, "Truncated");
        }

        const auto pageBytes = encode(page);
        raw.resize(0);
        raw.writeAllAt(0, fileBytes);
        raw.writeAllAt(fileBytes.size(),
                       std::span<const std::byte>{pageBytes}.first(pageBytes.size() - 1));
        JournalFile reader(path, Mode::ReadOnly);
        requireRecord(reader.readNext().value(), file);
        requireError([&] { reader.readNext(); }, "Truncated");
        requireError([&] { reader.readNext(); }, "Truncated");

        // A failed read must leave the cursor at the same record's start.
        raw.writeAllAt(fileBytes.size() + pageBytes.size() - 1,
                       std::span<const std::byte>{pageBytes}.last(1));
        requireRecord(reader.readNext().value(), page);
        require(!reader.readNext(), "Repaired record did not end at EOF");
    }

    void testInvalidRecords(const std::filesystem::path &directory)
    {
        const auto path = directory / "invalid";
        LinuxFile raw(path, Mode::CreateNew);
        auto check = [&](const Bytes &bytes, const std::string &message)
        {
            raw.resize(0);
            raw.writeAllAt(0, bytes);
            JournalFile reader(path, Mode::ReadOnly);
            requireError([&] { reader.readNext(); }, message);
            requireError([&] { reader.readNext(); }, message);
        };

        auto unknownType = encode(FileBeforeImage{"tables/one.table", PAGE_SIZE});
        unknownType[0] = std::byte{0xff};
        check(unknownType, "Record Type");

        for (const std::uint32_t length :
             {std::uint32_t{0}, static_cast<std::uint32_t>(JournalFile::MaxPathBytes + 1)})
        {
            Bytes bytes;
            appendUnsigned(bytes, static_cast<std::uint8_t>(JournalRecordType::FileBeforeImage));
            appendUnsigned(bytes, length);
            check(bytes, "path length");
        }
        check(encode(FileBeforeImage{"../outside", PAGE_SIZE}), "must not contain");
        auto invalidFlag = encode(FileBeforeImage{"tables/one.table", PAGE_SIZE});
        invalidFlag.back() = std::byte{2};
        check(invalidFlag, "new-file flag");
    }
}

int main()
{
    try
    {
        TestDirectory directory;
        testRecordBoundaries(directory.path);
        testTruncation(directory.path);
        testInvalidRecords(directory.path);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
