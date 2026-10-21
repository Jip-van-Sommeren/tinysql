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
        auto expectedBytes = journal_test::encodePrefix();
        for (const auto &record : {journal_test::encode(metadata, 0), journal_test::encode(first, 1),
                                  journal_test::encode(second, 2), journal_test::encode(first, 3)})
            expectedBytes.insert(expectedBytes.end(), record.begin(), record.end());
        {
            JournalFile journal(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId);
            journal_test::initialize(journal, expectedHeader);
            require(journal.writeFileBeforeImage(metadata) == JournalDataOffset + journal_test::padded(metadata.size()), "File append end changed");
            journal.writePage(first);
            journal.writePage(second);
            journal.writePage(first); // Preserve duplicate images and cross-file page IDs.
            journal.sync();
            require(journal_test::readFile(path) == expectedBytes, "Writer disagrees with version-2 wire format");
        }
        requireThrows<std::system_error>([&] { JournalFile duplicate(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId); }, "open");
        require(journal_test::readFile(path) == expectedBytes, "CreateNew overwrote an existing journal");
        JournalFile reader(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId, Mode::ReadOnly);
        for (int pass = 0; pass < 2; ++pass)
        {
            reader.rewind();
            const auto header = reader.readJournalHeader();
            require(header.magic == expectedHeader.magic && header.version == 2 && header.headerLength == 60 &&
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
            JournalFile writer(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId);
            journal_test::initialize(writer);
            writer.writeFileBeforeImage(first);
        }
        const auto before = journal_test::readFile(path);
        {
            JournalFile writer(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId, Mode::OpenExisting);
            (void)writer.readJournalHeader();
            requireRecord(writer.readNext().value(), first, 0);
            require(writer.writePage(second) == before.size() + journal_test::padded(second.size()), "Reopen did not append at EOF");
            requireRecord(writer.readNext().value(), second, 1);
        }
        const auto after = journal_test::readFile(path);
        require(std::equal(before.begin(), before.end(), after.begin()), "Append overwrote existing recovery data");
        JournalFile reader(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId, Mode::ReadOnly);
        (void)reader.readJournalHeader();
        requireRecord(reader.readNext().value(), first, 0);
        requireRecord(reader.readNext().value(), second, 1);
        require(!reader.readNext(), "Reopened append has extra bytes");
    }

    void testBoundaryUpdatesPreserveRecords(const std::filesystem::path &directory)
    {
        LinuxDirectory parent{directory};
        const auto path = directory / "boundary-writes";
        JournalFile journal(parent, path.filename(), journal_test::DatabaseId, journal_test::StatementId);
        journal_test::initialize(journal);
        const FileBeforeImage first{"one.table", PAGE_SIZE};
        const FileBeforeImage second{"two.table", PAGE_SIZE};
        const auto firstEnd = journal.writeFileBeforeImage(first);
        auto expected = journal_test::encodePrefix();
        const auto firstBytes = journal_test::encode(first);
        expected.insert(expected.end(), firstBytes.begin(), firstBytes.end());
        require(journal_test::readFile(path) == expected, "First append overwrote journal metadata");

        journal.writeJournalBoundary({1, firstEnd, 0}, 1);
        const auto boundary = journal_test::encodeBoundary(1, firstEnd);
        std::copy(boundary.begin(), boundary.end(), expected.begin() + journal_test::boundaryOffset(1));
        require(journal_test::readFile(path) == expected, "Boundary publication modified other journal bytes");
        require(journal.writeFileBeforeImage(second) == firstEnd + journal_test::padded(second.size()),
                "Boundary publication moved the append offset");
        const auto secondBytes = journal_test::encode(second, 1);
        expected.insert(expected.end(), secondBytes.begin(), secondBytes.end());
        require(journal_test::readFile(path) == expected, "Append after boundary publication lost data or sequence");
    }

    void testReopenedEmptyJournalValidatesHeader(const std::filesystem::path &directory)
    {
        LinuxDirectory parent{directory};
        const auto path = directory / "invalid-empty";
        auto bytes = journal_test::encodePrefix();
        bytes[0] ^= std::byte{1};
        LinuxFile raw(parent, path.filename(), Mode::CreateNew);
        raw.writeAllAt(0, bytes);
        JournalFile reopened(parent, path.filename(), journal_test::DatabaseId, journal_test::StatementId, Mode::OpenExisting);
        requireThrows<std::runtime_error>([&] { reopened.writeFileBeforeImage({"one.table", 0, true}); }, "magic");
        require(journal_test::readFile(path) == bytes, "Reopened empty journal skipped header validation");
    }

    void testReopenedAppendRejectsCorruption(const std::filesystem::path &directory)
    {
        const auto path = directory / "bad-append";
        const FileBeforeImage first{"tables/a.table", PAGE_SIZE};
        const auto goodRecord = journal_test::encode(first);
        for (const bool truncated : {false, true})
        {
            LinuxFile raw(LinuxDirectory{path.parent_path()}, path.filename(), truncated ? Mode::OpenExisting : Mode::CreateNew);
            raw.resize(0);
            raw.writeAllAt(0, journal_test::encodePrefix());
            auto badRecord = goodRecord;
            if (truncated)
                badRecord.pop_back();
            else
                badRecord.back() ^= std::byte{1};
            raw.writeAllAt(JournalDataOffset, badRecord);
            const auto before = journal_test::readFile(path);
            JournalFile writer(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId, Mode::OpenExisting);
            requireThrows<std::runtime_error>([&] { writer.writeFileBeforeImage(first); },
                                             truncated ? "Truncated" : "checksum");
            require(journal_test::readFile(path) == before, "Reopened append modified invalid recovery data");
        }
    }

    void testHeaderOnlyAndInvalidPaths(const std::filesystem::path &directory)
    {
        const auto path = directory / "empty";
        JournalFile journal(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId);
        journal_test::initialize(journal);
        (void)journal.readJournalHeader();
        require(!journal.readNext() && journal.findRecoveryEnd(journal.size()) == JournalDataOffset,
                "Header-only journal is not a valid empty statement");
        for (const auto &name : {std::string{}, std::string{"/absolute"}, std::string{"../outside"},
                                 std::string{"x\0y", 3}, std::string(JournalFile::MaxPathBytes + 1, 'x')})
        {
            requireThrows<std::runtime_error>([&] { journal.writePage(makeRecord(name, 0, std::byte{0})); });
            requireThrows<std::runtime_error>([&] { journal.writeFileBeforeImage({name, 0, true}); });
        }
        require(journal_test::readFile(path) == journal_test::encodePrefix(), "Rejected append wrote partial bytes");
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
            JournalFile writer(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId);
            journal_test::initialize(writer);
            writer.writePage(record);
        }
        LinuxFile raw(LinuxDirectory{path.parent_path()}, path.filename());
        raw.resize(1ULL << 30); // Sparse; reading one record must not allocate the entire journal.
        JournalFile reader(LinuxDirectory{directory}, path.filename(), journal_test::DatabaseId, journal_test::StatementId, Mode::ReadOnly);
        (void)reader.readJournalHeader();
        requireRecord(reader.readNext().value(), record, 0);
        requireThrows<std::runtime_error>([&] { reader.readNext(); }, "Record Type");
    }

    void testPageFile(const std::filesystem::path &directory)
    {
        const auto path = directory / "pages";
        requireThrows<std::system_error>([&] { PageFile missing(LinuxDirectory{path.parent_path()}, path.filename()); }, "open");
        require(!std::filesystem::exists(path), "PageFile reopen created missing file");
        PageFile pages(LinuxDirectory{path.parent_path()}, path.filename(), Mode::CreateNew);
        RawPage expected{};
        expected.bytes.fill(std::byte{0x9a});
        pages.writePage(3, expected);
        PageFile reopened(LinuxDirectory{path.parent_path()}, path.filename());
        require(reopened.readPage(3).bytes == expected.bytes, "Unflushed page write was not visible");
        require(readPageFromFile(LinuxDirectory{path.parent_path()}, path.filename(), 3).bytes == expected.bytes, "Standalone reader changed behavior");
        requireThrows<std::runtime_error>([&] { pages.readPage(4); }, "EOF");
        requireThrows<std::system_error>([&] { PageFile duplicate(LinuxDirectory{path.parent_path()}, path.filename(), Mode::CreateNew); }, "open");
        pages.sync();
        std::filesystem::create_directory(directory / "journal");
        std::filesystem::create_directory(directory / "tables");
        StatementRecovery recovery{LinuxDirectory{directory}, journal_test::DatabaseId};
        requireThrows<std::system_error>([&] { Table::open(LinuxDirectory{directory / "tables"}, "missing.table", recovery); }, "open");
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
        testBoundaryUpdatesPreserveRecords(directory.path);
        testReopenedEmptyJournalValidatesHeader(directory.path);
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
