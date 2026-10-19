#pragma once

#include "db_journal_file.h"

#include <algorithm>
#include <array>
#include <span>
#include <zlib.h>

// Independent encoders for the journal wire format. Do not use ByteWriter or
// the production journal writer: matching reader/writer bugs must be visible.
namespace journal_test
{
    using Bytes = std::vector<std::byte>;
    inline constexpr std::size_t HeaderSize = 60;
    inline constexpr std::size_t BoundarySize = 20;
    inline constexpr std::size_t DataOffset = HeaderSize + 2 * BoundarySize;
    inline constexpr std::size_t RecordHeaderSize = 17;
    inline constexpr std::size_t RecordChecksumOffset = 13;
    inline constexpr uuid_t DatabaseId{
        0x11, 0x23, 0x35, 0x47, 0x59, 0x6b, 0x4d, 0x8f,
        0x91, 0xa3, 0xb5, 0xc7, 0xd9, 0xeb, 0xfd, 0x0f};
    inline constexpr uuid_t StatementId{
        0xf1, 0xe2, 0xd3, 0xc4, 0xb5, 0xa6, 0x47, 0x88,
        0x99, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x60};

    static_assert(JournalHeaderBytes == HeaderSize);
    static_assert(JournalRecordHeaderBytes == RecordHeaderSize);
    static_assert(JournalBoundaryBytes == BoundarySize);
    static_assert(JournalDataOffset == DataOffset);

    template <typename T>
    void appendUnsigned(Bytes &bytes, T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    }

    template <typename T>
    void setUnsigned(Bytes &bytes, std::size_t offset, T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes.at(offset + i) = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
    }

    inline std::uint32_t crc(std::span<const std::byte> bytes)
    {
        return static_cast<std::uint32_t>(crc32_z(
            0, reinterpret_cast<const Bytef *>(bytes.data()), bytes.size()));
    }

    inline JournalHeader header()
    {
        JournalHeader value{};
        value.magic = {'D', 'B', 'J', 'R', 'N', 'L', '\0', '\0'};
        value.version = 1;
        value.headerLength = HeaderSize;
        uuid_copy(value.dbId, DatabaseId);
        uuid_copy(value.statementId, StatementId);
        value.pageSize = PAGE_SIZE;
        return value;
    }

    inline Bytes encodeHeader(const JournalHeader &value = header())
    {
        Bytes bytes;
        for (const auto byte : value.magic)
            bytes.push_back(static_cast<std::byte>(byte));
        appendUnsigned(bytes, value.version);
        appendUnsigned(bytes, value.headerLength);
        for (const auto byte : value.dbId)
            bytes.push_back(static_cast<std::byte>(byte));
        for (const auto byte : value.statementId)
            bytes.push_back(static_cast<std::byte>(byte));
        appendUnsigned(bytes, value.pageSize);
        appendUnsigned(bytes, crc(bytes));
        return bytes;
    }

    inline std::uint32_t identityChecksum(std::span<const std::byte> bytes,
                                         const JournalHeader &identity = header())
    {
        Bytes input;
        for (auto byte : identity.dbId)
            input.push_back(static_cast<std::byte>(byte));
        for (auto byte : identity.statementId)
            input.push_back(static_cast<std::byte>(byte));
        input.insert(input.end(), bytes.begin(), bytes.end());
        return crc(input);
    }

    inline Bytes encodeBoundary(std::uint64_t generation, std::uint64_t end,
                                const JournalHeader &identity = header())
    {
        Bytes bytes;
        appendUnsigned(bytes, generation);
        appendUnsigned(bytes, end);
        appendUnsigned(bytes, std::uint32_t{0});
        setUnsigned(bytes, 16, identityChecksum(bytes, identity));
        return bytes;
    }

    inline Bytes encodePrefix(const JournalHeader &identity = header())
    {
        auto bytes = encodeHeader(identity);
        const auto boundary = encodeBoundary(0, DataOffset, identity);
        for (int slot = 0; slot < 2; ++slot)
            bytes.insert(bytes.end(), boundary.begin(), boundary.end());
        return bytes;
    }

    inline void initialize(JournalFile &journal, const JournalHeader &identity = header())
    {
        journal.writeJournalHeader(identity);
        journal.writeJournalBoundary({0, DataOffset, 0}, 0);
        journal.writeJournalBoundary({0, DataOffset, 0}, 1);
    }

    inline void appendPath(Bytes &bytes, const std::filesystem::path &path)
    {
        const auto text = path.generic_string();
        appendUnsigned(bytes, static_cast<std::uint32_t>(text.size()));
        for (const auto byte : text)
            bytes.push_back(static_cast<std::byte>(byte));
    }

    inline void refreshChecksum(Bytes &bytes, const JournalHeader &identity = header())
    {
        setUnsigned(bytes, RecordChecksumOffset, std::uint32_t{0});
        setUnsigned(bytes, RecordChecksumOffset, identityChecksum(bytes, identity));
    }

    inline Bytes frame(JournalRecordType type, const Bytes &payload, std::uint32_t sequence)
    {
        Bytes bytes;
        appendUnsigned(bytes, static_cast<std::uint8_t>(type));
        appendUnsigned(bytes, std::uint64_t{RecordHeaderSize + payload.size()});
        appendUnsigned(bytes, sequence);
        appendUnsigned(bytes, std::uint32_t{0});
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        refreshChecksum(bytes);
        return bytes;
    }

    inline Bytes encode(const FileBeforeImage &value, std::uint32_t sequence = 0)
    {
        Bytes payload;
        appendPath(payload, value.relativeFilePath);
        appendUnsigned(payload, value.originalSize);
        appendUnsigned(payload, static_cast<std::uint8_t>(value.newFile));
        return frame(JournalRecordType::FileBeforeImage, payload, sequence);
    }

    inline Bytes encode(const PageBeforeImage &value, std::uint32_t sequence = 0)
    {
        Bytes payload;
        appendPath(payload, value.relativeFilePath);
        appendUnsigned(payload, value.pageId);
        payload.insert(payload.end(), value.originalPage.bytes.begin(), value.originalPage.bytes.end());
        return frame(JournalRecordType::PageBeforeImage, payload, sequence);
    }

    inline Bytes readFile(const std::filesystem::path &path)
    {
        LinuxFile file(LinuxDirectory{path.parent_path()}, path.filename(), LinuxFile::OpenMode::ReadOnly);
        Bytes bytes(file.size());
        file.readExactAt(0, bytes);
        return bytes;
    }

    struct DatabaseIdentity
    {
        uuid_t bytes{};
        explicit DatabaseIdentity(const std::filesystem::path &root)
        {
            LinuxFile meta(LinuxDirectory{root}, "db.meta", LinuxFile::OpenMode::ReadOnly);
            std::array<std::byte, 16> id{};
            meta.readExactAt(8, id);
            for (std::size_t i = 0; i < id.size(); ++i)
                bytes[i] = std::to_integer<unsigned char>(id[i]);
        }
    };
}
