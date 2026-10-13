#include "db_journal_file.h"
#include "db_test_directory.h"
#include "db_test_journal.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
    using namespace journal_test;
    using Mode = LinuxFile::OpenMode;

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <typename Function>
    void requireError(Function function, const std::string &message = "")
    {
        try { function(); }
        catch (const std::exception &error)
        {
            require(std::string{error.what()}.find(message) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected error: " + message);
    }

    void writeFixture(LinuxFile &file, std::span<const std::byte> records)
    {
        file.resize(0);
        file.writeAllAt(0, encodeHeader());
        file.writeAllAt(HeaderSize, records);
    }

    void requireRecord(const JournalRecord &actual, const FileBeforeImage &expected)
    {
        const auto *record = std::get_if<FileBeforeImage>(&actual.record);
        require(record && record->relativeFilePath == expected.relativeFilePath &&
                    record->originalSize == expected.originalSize && record->newFile == expected.newFile,
                "File before-image changed");
    }

    void requireRecord(const JournalRecord &actual, const PageBeforeImage &expected)
    {
        const auto *record = std::get_if<PageBeforeImage>(&actual.record);
        require(record && record->relativeFilePath == expected.relativeFilePath && record->pageId == expected.pageId &&
                    record->originalPage.bytes == expected.originalPage.bytes, "Page before-image changed");
    }

    void testRecordBoundaries(const std::filesystem::path &directory)
    {
        const FileBeforeImage file{"tables/one.table", (1ULL << 40) + PAGE_SIZE};
        PageBeforeImage page{"tables/one.table", 17, {}};
        for (std::size_t i = 0; i < page.originalPage.size(); ++i)
            page.originalPage.bytes[i] = static_cast<std::byte>(i % 256);
        const auto path = directory / "records";
        LinuxFile raw(path, Mode::CreateNew);
        writeFixture(raw, {});
        JournalFile reader(path, Mode::ReadOnly);
        (void)reader.readJournalHeader();
        require(!reader.readNext(), "Header-only journal should return EOF");
        raw.writeAllAt(HeaderSize, encode(file));
        requireRecord(reader.readNext().value(), file);
        require(!reader.readNext(), "File record did not end at EOF");
        raw.writeAllAt(raw.size(), encode(page, 1));
        requireRecord(reader.readNext().value(), page);
        require(!reader.readNext(), "Reader did not resume after EOF");
        for (int pass = 0; pass < 2; ++pass)
        {
            reader.rewind();
            (void)reader.readJournalHeader();
            requireRecord(reader.readNext().value(), file);
            requireRecord(reader.readNext().value(), page);
            require(!reader.readNext() && !reader.readNext(), "EOF is not stable");
        }
        require(reader.findRecoveryEnd(raw.size()) == raw.size(), "Recovery scan changed record boundary");
        (void)reader.readJournalHeader();
        requireRecord(reader.readNext().value(), file);
        requireRecord(reader.readNext().value(), page);
        for (const auto length : {std::size_t{1}, JournalFile::MaxPathBytes})
        {
            const FileBeforeImage boundaryFile{std::string(length, 'x'), 0, true};
            const PageBeforeImage boundaryPage{std::string(length, 'x'), 0, {}};
            writeFixture(raw, encode(boundaryFile));
            (void)reader.readJournalHeader();
            requireRecord(reader.readNext().value(), boundaryFile);
            writeFixture(raw, encode(boundaryPage));
            (void)reader.readJournalHeader();
            requireRecord(reader.readNext().value(), boundaryPage);
        }
    }

    void testHeaders(const std::filesystem::path &directory)
    {
        const auto path = directory / "headers";
        LinuxFile raw(path, Mode::CreateNew);
        const auto good = encodeHeader();
        for (std::size_t size = 0; size < HeaderSize; ++size)
        {
            raw.resize(0);
            raw.writeAllAt(0, std::span<const std::byte>{good}.first(size));
            JournalFile reader(path, Mode::ReadOnly);
            requireError([&] { reader.readJournalHeader(); }, "EOF");
            requireError([&] { reader.findRecoveryEnd(raw.size()); }, "EOF");
        }
        const auto check = [&](const Bytes &bytes, const std::string &message)
        {
            raw.resize(0);
            raw.writeAllAt(0, bytes);
            JournalFile reader(path, Mode::ReadOnly);
            requireError([&] { reader.readJournalHeader(); }, message);
            requireError([&] { reader.findRecoveryEnd(raw.size()); }, message);
        };
        auto value = header();
        value.magic[0] = 'X';
        check(encodeHeader(value), "magic");
        value = header(); value.version = 2;
        check(encodeHeader(value), "version");
        value = header(); value.headerLength = 59;
        check(encodeHeader(value), "length");
        value = header(); value.pageSize = PAGE_SIZE * 2;
        check(encodeHeader(value), "page size");
        auto corrupted = good;
        corrupted[20] ^= std::byte{1}; // A UUID byte, without updating CRC.
        check(corrupted, "checksum");
        raw.resize(0);
        raw.writeAllAt(0, good);
        JournalFile reader(path, Mode::ReadOnly);
        require(reader.findRecoveryEnd(raw.size()) == HeaderSize, "Empty statement rejected");
    }

    void testTruncation(const std::filesystem::path &directory)
    {
        const FileBeforeImage file{"tables/one.table", 2 * PAGE_SIZE};
        const PageBeforeImage page{"tables/one.table", 1, {}};
        const auto path = directory / "truncated";
        LinuxFile raw(path, Mode::CreateNew);
        const auto fileBytes = encode(file);
        for (std::size_t size = 1; size < fileBytes.size(); ++size)
        {
            writeFixture(raw, std::span<const std::byte>{fileBytes}.first(size));
            JournalFile reader(path, Mode::ReadOnly);
            (void)reader.readJournalHeader();
            requireError([&] { reader.readNext(); }, "Truncated");
            requireError([&] { reader.readNext(); }, "Truncated");
            requireError([&] { reader.findRecoveryEnd(raw.size()); }, "Truncated");
        }
        writeFixture(raw, fileBytes);
        const auto pageStart = raw.size();
        const auto pageBytes = encode(page, 1);
        raw.writeAllAt(pageStart, std::span<const std::byte>{pageBytes}.first(pageBytes.size() - 1));
        JournalFile reader(path, Mode::ReadOnly);
        (void)reader.readJournalHeader();
        requireRecord(reader.readNext().value(), file);
        requireError([&] { reader.readNext(); }, "Truncated");
        requireError([&] { reader.readNext(); }, "Truncated");
        raw.writeAllAt(pageStart + pageBytes.size() - 1, std::span<const std::byte>{pageBytes}.last(1));
        requireRecord(reader.readNext().value(), page);
        require(!reader.readNext(), "Repaired record did not end at EOF");
    }

    void testInvalidRecords(const std::filesystem::path &directory)
    {
        const auto path = directory / "invalid";
        LinuxFile raw(path, Mode::CreateNew);
        const auto check = [&](const Bytes &bytes, const std::string &message = "")
        {
            writeFixture(raw, bytes);
            JournalFile reader(path, Mode::ReadOnly);
            (void)reader.readJournalHeader();
            requireError([&] { reader.readNext(); }, message);
            requireError([&] { reader.readNext(); }, message);
            requireError([&] { reader.findRecoveryEnd(raw.size()); }, message);
        };
        const FileBeforeImage file{"tables/one.table", PAGE_SIZE};
        auto bytes = encode(file);
        bytes[0] = std::byte{0xff}; refreshChecksum(bytes);
        check(bytes, "Record Type");
        for (const auto &name : {std::string{}, std::string{"../outside"}, std::string{"/absolute"},
                                 std::string{"x\0y", 3}})
            check(encode(FileBeforeImage{name, PAGE_SIZE}), name.empty() ? "record size" : "path");
        bytes = encode(file);
        bytes.back() = std::byte{2}; refreshChecksum(bytes);
        check(bytes, "new-file flag");
        bytes = encode(file);
        bytes[RecordHeaderSize + 4] ^= std::byte{1};
        check(bytes, "checksum");
        bytes = encode(PageBeforeImage{"tables/one.table", 0, {}});
        bytes.back() ^= std::byte{1};
        check(bytes, "checksum");
        bytes = encode(file);
        bytes[RecordChecksumOffset] ^= std::byte{1};
        check(bytes, "checksum");
        check(encode(file, 1), "sequence");
        bytes = encode(file);
        bytes.push_back(std::byte{0});
        setUnsigned(bytes, 1, static_cast<std::uint64_t>(bytes.size()));
        refreshChecksum(bytes);
        check(bytes, "trailing bytes");
        bytes = encode(file);
        setUnsigned(bytes, RecordHeaderSize, std::numeric_limits<std::uint32_t>::max());
        refreshChecksum(bytes);
        check(bytes); // A forged string length must not overrun the bounded record.
        for (const auto type : {JournalRecordType::FileBeforeImage, JournalRecordType::PageBeforeImage})
        {
            for (const auto size : {std::uint64_t{0}, std::uint64_t{16},
                                   std::numeric_limits<std::uint64_t>::max()})
            {
                bytes = encode(file);
                bytes[0] = static_cast<std::byte>(type);
                setUnsigned(bytes, 1, size);
                refreshChecksum(bytes);
                check(bytes); // Reject impossible sizes before allocating or zeroing CRC bytes.
            }
            const auto maximum = type == JournalRecordType::FileBeforeImage ?
                JournalFile::maxFileRecord : JournalFile::maxPageRecord;
            bytes = encode(file);
            bytes.resize(maximum + 1);
            bytes[0] = static_cast<std::byte>(type);
            setUnsigned(bytes, 1, static_cast<std::uint64_t>(bytes.size()));
            refreshChecksum(bytes);
            check(bytes);
        }
    }

    void testSequenceAndChecksumRetry(const std::filesystem::path &directory)
    {
        const auto path = directory / "retry";
        LinuxFile raw(path, Mode::CreateNew);
        const FileBeforeImage file{"tables/one.table", PAGE_SIZE};
        const PageBeforeImage page{"tables/one.table", 0, {}};
        const auto first = encode(file);
        const auto second = encode(page, 1);
        const auto secondStart = HeaderSize + first.size();
        for (const bool badSequence : {false, true})
        {
            writeFixture(raw, first);
            auto bad = badSequence ? encode(page, 0) : second;
            if (!badSequence) bad.back() ^= std::byte{1};
            raw.writeAllAt(secondStart, bad);
            JournalFile reader(path, Mode::ReadOnly);
            (void)reader.readJournalHeader();
            requireRecord(reader.readNext().value(), file);
            requireError([&] { reader.readNext(); }, badSequence ? "sequence" : "checksum");
            raw.writeAllAt(secondStart, second);
            requireRecord(reader.readNext().value(), page);
            require(!reader.readNext(), "Failed validation advanced read state");
        }
    }
}

int main()
{
    try
    {
        TestDirectory directory{"db-journal-reader"};
        testRecordBoundaries(directory.path);
        testHeaders(directory.path);
        testTruncation(directory.path);
        testInvalidRecords(directory.path);
        testSequenceAndChecksumRetry(directory.path);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
