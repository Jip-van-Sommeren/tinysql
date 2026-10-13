#include "db_journal_file.h"
#include "db_page_file.h"
#include "db_read.h"
#include "db_table.h"
#include "db_test_directory.h"
#include "db_test_journal.h"

#include <iostream>
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
        try { function(); }
        catch (const Error &error)
        {
            require(std::string{error.what()}.find(message) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected exception: " + message);
    }

    PageBeforeImage makeRecord(const std::string &path, std::uint32_t pageId, std::byte value)
    {
        PageBeforeImage result{path, pageId, {}};
        result.originalPage.bytes.fill(value);
        return result;
    }

    void requireRecord(const JournalRecord &actual, const PageBeforeImage &expected, std::uint32_t sequence)
    {
        require(actual.header.type == JournalRecordType::PageBeforeImage &&
                    actual.header.sequenceNumber == sequence && actual.header.recordSize == expected.size(),
                "Page record header changed");
        const auto &record = std::get<PageBeforeImage>(actual.record);
        require(record.relativeFilePath == expected.relativeFilePath && record.pageId == expected.pageId &&
                    record.originalPage.bytes == expected.originalPage.bytes, "Page record changed");
    }

    void requireRecord(const JournalRecord &actual, const FileBeforeImage &expected, std::uint32_t sequence)
    {
        require(actual.header.type == JournalRecordType::FileBeforeImage &&
                    actual.header.sequenceNumber == sequence && actual.header.recordSize == expected.size(),
                "File record header changed");
        const auto &record = std::get<FileBeforeImage>(actual.record);
        require(record.relativeFilePath == expected.relativeFilePath && record.originalSize == expected.originalSize &&
                    record.newFile == expected.newFile, "File record changed");
    }

    void testRoundTrip(const std::filesystem::path &directory)
    {
        const auto path = directory / "undo.journal";
        const auto first = makeRecord("tables/one.table", 1, std::byte{0xa1});
        const auto second = makeRecord("tables/two.table", 1, std::byte{0xb2});
        const FileBeforeImage metadata{"tables/one.table", (1ULL << 40) + PAGE_SIZE};
        const auto expectedHeader = journal_test::header();
        auto expectedBytes = journal_test::encodeHeader();
        for (const auto &record : {journal_test::encode(metadata, 0), journal_test::encode(first, 1),
                                  journal_test::encode(second, 2), journal_test::encode(first, 3)})
            expectedBytes.insert(expectedBytes.end(), record.begin(), record.end());
        {
            JournalFile journal(path);
            require(journal.writeJournalHeader(expectedHeader) == 60, "Header append end changed");
            require(journal.writeFileBeforeImage(metadata) == 60 + metadata.size(), "File append end changed");
            journal.writePage(first);
            journal.writePage(second);
            journal.writePage(first); // Preserve duplicate images and cross-file page IDs.
            journal.sync();
            require(journal_test::readFile(path) == expectedBytes, "Writer disagrees with version-1 wire format");
        }
        requireThrows<std::system_error>([&] { JournalFile duplicate(path); }, "open");
        require(journal_test::readFile(path) == expectedBytes, "CreateNew overwrote an existing journal");
        JournalFile reader(path, Mode::ReadOnly);
        for (int pass = 0; pass < 2; ++pass)
        {
            reader.rewind();
            const auto header = reader.readJournalHeader();
            require(header.magic == expectedHeader.magic && header.version == 1 && header.headerLength == 60 &&
                        header.pageSize == PAGE_SIZE && uuid_compare(header.dbId, expectedHeader.dbId) == 0 &&
                        uuid_compare(header.statementId, expectedHeader.statementId) == 0,
                    "Journal header or UUID bytes changed");
            requireRecord(reader.readNext().value(), metadata, 0);
            requireRecord(reader.readNext().value(), first, 1);
            requireRecord(reader.readNext().value(), second, 2);
            requireRecord(reader.readNext().value(), first, 3);
            require(!reader.readNext() && !reader.readNext(), "Journal EOF is not stable");
        }
        require(reader.findRecoveryEnd(reader.size()) == expectedBytes.size(), "Recovery scan end changed");
        (void)reader.readJournalHeader();
        requireRecord(reader.readNext().value(), metadata, 0);
        requireThrows<std::system_error>([&] { reader.writePage(first); }, "pwrite");
    }

    void testReopenedAppend(const std::filesystem::path &directory)
    {
        const auto path = directory / "append";
        const FileBeforeImage first{"tables/a.table", PAGE_SIZE};
        const auto second = makeRecord("tables/a.table", 0, std::byte{0x19});
        {
            JournalFile writer(path);
            writer.writeJournalHeader(journal_test::header());
            writer.writeFileBeforeImage(first);
        }
        const auto before = journal_test::readFile(path);
        {
            JournalFile writer(path, Mode::OpenExisting);
            (void)writer.readJournalHeader();
            requireRecord(writer.readNext().value(), first, 0);
            require(writer.writePage(second) == before.size() + second.size(), "Reopen did not append at EOF");
            requireRecord(writer.readNext().value(), second, 1);
        }
        const auto after = journal_test::readFile(path);
        require(std::equal(before.begin(), before.end(), after.begin()), "Append overwrote existing recovery data");
        JournalFile reader(path, Mode::ReadOnly);
        (void)reader.readJournalHeader();
        requireRecord(reader.readNext().value(), first, 0);
        requireRecord(reader.readNext().value(), second, 1);
        require(!reader.readNext(), "Reopened append has extra bytes");
    }

    void testReopenedAppendRejectsCorruption(const std::filesystem::path &directory)
    {
        const auto path = directory / "bad-append";
        const FileBeforeImage first{"tables/a.table", PAGE_SIZE};
        const auto goodRecord = journal_test::encode(first);
        for (const bool truncated : {false, true})
        {
            LinuxFile raw(path, truncated ? Mode::OpenExisting : Mode::CreateNew);
            raw.resize(0);
            raw.writeAllAt(0, journal_test::encodeHeader());
            auto badRecord = goodRecord;
            if (truncated)
                badRecord.pop_back();
            else
                badRecord.back() ^= std::byte{1};
            raw.writeAllAt(JournalHeaderBytes, badRecord);
            const auto before = journal_test::readFile(path);
            JournalFile writer(path, Mode::OpenExisting);
            requireThrows<std::runtime_error>([&] { writer.writeFileBeforeImage(first); },
                                             truncated ? "Truncated" : "checksum");
            require(journal_test::readFile(path) == before, "Reopened append modified invalid recovery data");
        }
    }

    void testHeaderOnlyAndInvalidPaths(const std::filesystem::path &directory)
    {
        const auto path = directory / "empty";
        JournalFile journal(path);
        journal.writeJournalHeader(journal_test::header());
        (void)journal.readJournalHeader();
        require(!journal.readNext() && journal.findRecoveryEnd(journal.size()) == 60,
                "Header-only journal is not a valid empty statement");
        for (const auto &name : {std::string{}, std::string{"/absolute"}, std::string{"../outside"},
                                 std::string{"x\0y", 3}, std::string(JournalFile::MaxPathBytes + 1, 'x')})
        {
            requireThrows<std::runtime_error>([&] { journal.writePage(makeRecord(name, 0, std::byte{0})); });
            requireThrows<std::runtime_error>([&] { journal.writeFileBeforeImage({name, 0, true}); });
        }
        require(journal_test::readFile(path) == journal_test::encodeHeader(), "Rejected append wrote partial bytes");
        const FileBeforeImage valid{"tables/new.table", 0, true};
        journal.writeFileBeforeImage(valid);
        (void)journal.readJournalHeader();
        requireRecord(journal.readNext().value(), valid, 0);
    }

    void testLargeJournalStreaming(const std::filesystem::path &directory)
    {
        const auto path = directory / "large";
        const auto record = makeRecord("tables/one.table", 1, std::byte{0x71});
        {
            JournalFile writer(path);
            writer.writeJournalHeader(journal_test::header());
            writer.writePage(record);
        }
        LinuxFile raw(path);
        raw.resize(1ULL << 30); // Sparse; reading one record must not allocate the entire journal.
        JournalFile reader(path, Mode::ReadOnly);
        (void)reader.readJournalHeader();
        requireRecord(reader.readNext().value(), record, 0);
        requireThrows<std::runtime_error>([&] { reader.readNext(); }, "Record Type");
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
        StatementRecovery recovery{directory, journal_test::DatabaseId};
        requireThrows<std::system_error>([&] { Table::open(directory, "missing.table", recovery); }, "open");
        require(!std::filesystem::exists(directory / "missing.table"), "Table::open created a missing table");
    }
}

int main()
{
    try
    {
        TestDirectory directory{"db-journal-test"};
        testRoundTrip(directory.path);
        testReopenedAppend(directory.path);
        testReopenedAppendRejectsCorruption(directory.path);
        testHeaderOnlyAndInvalidPaths(directory.path);
        testLargeJournalStreaming(directory.path);
        testPageFile(directory.path);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
