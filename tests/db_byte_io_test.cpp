#include "db_read.h"
#include "db_write.h"

#include "db_page_factory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void require(bool condition, const std::string &message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename Error, typename Function>
    void requireThrows(Function function)
    {
        try
        {
            function();
        }
        catch (const Error &)
        {
            return;
        }
        throw std::runtime_error("Expected byte I/O error");
    }

    template <typename T>
    void testUnsignedRoundTrips()
    {
        for (T value : std::array<T, 3>{0, 1, std::numeric_limits<T>::max()})
        {
            ByteWriter writer(sizeof(T) + 2);
            writer.seek(1);
            writer.writeUnsigned<T>(value);
            require(writer.position() == 1 + sizeof(T), "Wrong unsigned write position");
            require(writer.bytes().front() == std::byte{0} &&
                        writer.bytes().back() == std::byte{0},
                    "Unsigned write changed adjacent bytes");

            ByteReader reader(writer.bytes());
            reader.seek(1);
            require(reader.readUnsignedAt<T>(1) == value, "Unsigned offset read failed");
            require(reader.position() == 1, "Offset read moved the position");
            require(reader.readUnsigned<T>() == value, "Unsigned round trip failed");
            require(reader.position() == 1 + sizeof(T), "Wrong unsigned read position");

            writer.writeUnsignedAt<T>(1, 0);
            require(writer.position() == 1 + sizeof(T), "Offset write moved the position");
            require(reader.readUnsignedAt<T>(1) == 0, "Reader did not borrow the buffer");
        }
    }

    void testUnsignedEncoding()
    {
        // These template calls live outside db_read.cpp/db_write.cpp so they
        // also verify that the template definitions are available to callers.
        ByteWriter writer(15);
        writer.writeUnsigned<std::uint8_t>(0xef);
        writer.writeUnsigned<std::uint16_t>(0xabcd);
        writer.writeUnsigned<std::uint32_t>(0x12345678);
        writer.writeUnsigned<std::uint64_t>(0x1122334455667788ULL);
        const std::array<std::uint8_t, 15> expected{
            0xef, 0xcd, 0xab, 0x78, 0x56, 0x34, 0x12,
            0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            require(writer.bytes()[i] == static_cast<std::byte>(expected[i]),
                    "Unsigned encoding is not little-endian");
        }
        ByteReader reader(writer.bytes());
        require(reader.readUnsigned<std::uint8_t>() == 0xef, "uint8 decoding failed");
        require(reader.readUnsigned<std::uint16_t>() == 0xabcd, "uint16 decoding failed");
        require(reader.readUnsigned<std::uint32_t>() == 0x12345678, "uint32 decoding failed");
        require(reader.readUnsigned<std::uint64_t>() == 0x1122334455667788ULL,
                "uint64 decoding failed");
        require(reader.position() == reader.size(), "Exact-size read failed");

        testUnsignedRoundTrips<std::uint8_t>();
        testUnsignedRoundTrips<std::uint16_t>();
        testUnsignedRoundTrips<std::uint32_t>();
        testUnsignedRoundTrips<std::uint64_t>();
    }

    void testStringsAndLargeBuffers()
    {
        const std::string embedded{"a\0b", 3};
        const std::string longString(PAGE_SIZE + 17, 'x');
        ByteWriter writer(4 + 4 + embedded.size() + 4 + longString.size());
        writer.writeString("");
        writer.writeString(embedded);
        writer.writeString(longString);
        require(writer.position() == writer.size(), "Wrong string write position");
        require(writer.bytes()[4] == std::byte{3} &&
                    writer.bytes()[5] == std::byte{0} &&
                    writer.bytes()[6] == std::byte{0} &&
                    writer.bytes()[7] == std::byte{0},
                "String length prefix changed");
        ByteReader reader(writer.bytes());
        require(reader.readString().empty(), "Empty string failed");
        require(reader.readString() == embedded, "Embedded null was lost");
        require(reader.readString() == longString, "Reader still has a page-size limit");
        require(reader.position() == reader.size(), "Wrong string read position");

        // A journal-shaped payload, without depending on unfinished file I/O.
        const std::string path = "tables/items.db";
        RawPage original{};
        for (std::size_t i = 0; i < original.size(); ++i)
        {
            original[i] = static_cast<std::byte>(i % 256);
        }
        ByteWriter record(4 + path.size() + 4 + original.size());
        record.writeString(path);
        record.writeUnsigned<std::uint32_t>(42);
        record.writeBytes(original.data(), original.size());
        ByteReader recordReader(record.bytes());
        require(recordReader.readString() == path, "Journal-shaped path failed");
        require(recordReader.readUnsigned<std::uint32_t>() == 42, "Journal-shaped ID failed");
        RawPage restored{};
        recordReader.readBytes(restored.data(), restored.size());
        require(restored.bytes == original.bytes, "Large raw byte transfer failed");
        require(recordReader.position() == record.size(), "Record size mismatch");
    }

    void testWriterFailures()
    {
        ByteWriter writer(8);
        writer.writeUnsigned<std::uint64_t>(0x1122334455667788ULL);
        writer.seek(2);
        const auto before = writer.bytes();
        auto rejected = [&](auto operation)
        {
            const auto position = writer.position();
            requireThrows<std::out_of_range>(operation);
            require(writer.position() == position, "Failed write moved the position");
            require(writer.bytes() == before, "Failed write changed the buffer");
        };

        constexpr auto maxSize = std::numeric_limits<std::size_t>::max();
        rejected([&] { writer.seek(9); });
        rejected([&] { writer.seek(maxSize); });
        rejected([&] { writer.writeUnsigned<std::uint64_t>(1); });
        rejected([&] { writer.writeUnsignedAt<std::uint32_t>(7, 1); });
        rejected([&] { writer.writeUnsignedAt<std::uint32_t>(maxSize, 1); });
        rejected([&] { writer.writeBytes(before.data(), maxSize); });
        rejected([&] { writer.writeBytesAt(maxSize, nullptr, 0); });
        rejected([&] { writer.writeBytesAt(2, before.data(), maxSize); });
        rejected([&] { writer.writeString("abc"); }); // Prefix fits, payload does not.
        writer.seek(7);
        rejected([&] { writer.writeString(""); }); // Prefix itself is truncated.

        writer.seek(2);
        requireThrows<std::invalid_argument>([&] { writer.writeBytes(nullptr, 1); });
        requireThrows<std::invalid_argument>([&] { writer.writeBytesAt(0, nullptr, 1); });
        require(writer.position() == 2 && writer.bytes() == before,
                "Null-source failure changed writer state");

        writer.seek(writer.size());
        writer.writeBytes(nullptr, 0);
        writer.writeBytesAt(writer.size(), nullptr, 0);
        rejected([&] { writer.writeUnsigned<std::uint8_t>(0); });
        require(writer.size() == 8 && writer.bytes().size() == 8, "Writer grew its buffer");
    }

    void testReaderFailures()
    {
        const std::array<std::byte, 8> buffer{};
        ByteReader reader(buffer);
        reader.seek(2);
        auto rejected = [&](auto operation)
        {
            const auto position = reader.position();
            requireThrows<std::out_of_range>(operation);
            require(reader.position() == position, "Failed read moved the position");
        };
        constexpr auto maxSize = std::numeric_limits<std::size_t>::max();
        rejected([&] { reader.seek(9); });
        rejected([&] { reader.seek(maxSize); });
        rejected([&] { reader.readUnsigned<std::uint64_t>(); });
        rejected([&] { reader.readUnsignedAt<std::uint32_t>(7); });
        rejected([&] { reader.readUnsignedAt<std::uint32_t>(maxSize); });
        rejected([&] { reader.readBytes(maxSize); }); // Must validate before allocating.
        rejected([&] { reader.readBytesAt(2, maxSize); });
        rejected([&] { reader.readBytesAt(maxSize, 0); });
        std::array<std::byte, 8> output{};
        output.fill(std::byte{0xab});
        const auto untouched = output;
        rejected([&] { reader.readBytes(output.data(), output.size()); });
        require(output == untouched, "Failed read changed the destination");
        requireThrows<std::invalid_argument>([&] { reader.readBytes(nullptr, 1); });
        require(reader.position() == 2, "Null-destination failure moved the position");

        reader.seek(reader.size());
        reader.readBytes(nullptr, 0);
        require(reader.readBytes(0).empty(), "Zero-size read at end failed");
        require(reader.readBytesAt(reader.size(), 0).empty(), "Zero-size offset read failed");
        rejected([&] { reader.readUnsigned<std::uint8_t>(); });

        // Reading a truncated string must not consume its length prefix.
        ByteWriter strings(8);
        strings.writeUnsignedAt<std::uint32_t>(1, 4);
        ByteReader truncated(strings.bytes());
        truncated.seek(1);
        requireThrows<std::out_of_range>([&] { truncated.readString(); });
        require(truncated.position() == 1, "Truncated string consumed its prefix");
        strings.writeUnsignedAt<std::uint32_t>(1, std::numeric_limits<std::uint32_t>::max());
        requireThrows<std::out_of_range>([&] { truncated.readString(); });
        require(truncated.position() == 1, "Oversized string consumed its prefix");

        for (std::size_t length = 0; length < sizeof(std::uint32_t); ++length)
        {
            ByteReader prefix(std::span<const std::byte>{buffer}.first(length));
            requireThrows<std::out_of_range>([&] { prefix.readString(); });
            require(prefix.position() == 0, "Incomplete prefix moved the position");
        }
    }

    void testEmptyAndRawBuffers()
    {
        ByteWriter empty(0);
        empty.seek(0);
        empty.writeBytes(nullptr, 0);
        empty.writeBytesAt(0, nullptr, 0);
        requireThrows<std::out_of_range>([&] { empty.seek(1); });
        requireThrows<std::out_of_range>([&] { empty.writeString(""); });
        requireThrows<std::out_of_range>([&] { empty.writeUnsigned<std::uint8_t>(0); });
        ByteReader emptyReader(std::span<const std::byte>{});
        emptyReader.seek(0);
        emptyReader.readBytes(nullptr, 0);
        require(emptyReader.readBytes(0).empty(), "Empty buffer read failed");
        require(emptyReader.readBytesAt(0, 0).empty(), "Empty buffer offset read failed");
        requireThrows<std::out_of_range>([&] { emptyReader.seek(1); });
        requireThrows<std::out_of_range>([&] { emptyReader.readUnsigned<std::uint8_t>(); });

        const std::array<std::byte, 8> bytes{
            std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
            std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
        ByteWriter writer(bytes.size());
        writer.writeBytes(bytes.data(), bytes.size());
        writer.seek(1);
        writer.writeBytesAt(2, writer.bytes().data(), 6);
        const std::vector<std::byte> overlapped{
            std::byte{1}, std::byte{2}, std::byte{1}, std::byte{2},
            std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}};
        require(writer.bytes() == overlapped, "Overlapping write corrupted the source");
        require(writer.position() == 1, "Offset raw write moved the position");

        ByteReader reader(writer.bytes());
        reader.seek(2);
        require(reader.readBytesAt(0, writer.size()) == overlapped, "Offset raw read failed");
        require(reader.position() == 2, "Offset raw read moved the position");
        require(reader.readBytes(6) == std::vector<std::byte>(bytes.begin(), bytes.begin() + 6),
                "Sequential vector read failed");
        require(reader.position() == 8, "Raw read did not advance");

        writer.writeBytes(writer.bytes().data() + 2, 6);
        require(writer.position() == 7, "Sequential overlapping write did not advance");
        require(std::equal(bytes.begin(), bytes.begin() + 6, writer.bytes().begin() + 1),
                "Sequential overlapping write failed");
    }

    std::uint64_t pageHash(const RawPage &page)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::byte byte : page.bytes)
        {
            hash ^= std::to_integer<std::uint8_t>(byte);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    void testPageCompatibility()
    {
        const Page header = makeHeaderPage("items", "MYDB", {
            Column{.name = "id", .type = DataType::Int, .nullable = false,
                   .columnIndex = 0, .storage = FixedColumnStorage{}},
            Column{.name = "name", .type = DataType::Text, .nullable = true,
                   .columnIndex = 1, .storage = VarColumnStorage{}}}, {});
        const HeaderPage &schema = std::get<HeaderPage>(header.data);
        const std::vector<Row> rows{
            Row{{std::int32_t{0x12345678}, std::string{"a\0b", 3}}},
            Row{{std::int32_t{-7}, std::monostate{}}}};

        // Full-page FNV-1a hashes captured from the pre-refactor encoders.
        // Cover padding and the slot directory, not just sequential writes.
        const RawPage headerBytes = encodePage(header);
        require(pageHash(headerBytes) == 10188868826003330422ULL, "Header page format changed");
        const Page decodedHeader = decodeHeaderPage(headerBytes);
        require(encodePage(decodedHeader).bytes == headerBytes.bytes, "Header round trip failed");
        require(decodedHeader.header.freeSpaceStart == header.header.freeSpaceStart,
                "Header free-space calculation changed");

        const RawPage data = encodeDataPage(0x10203040, schema, rows);
        require(pageHash(data) == 303497842505284136ULL, "Data page format changed");
        Page decoded = decodeDataPage(data, schema);
        require(decoded.header.pageId == 0x10203040 && decoded.header.slotCount == 2,
                "Data header decoding failed");
        DataPage &decodedData = std::get<DataPage>(decoded.data);
        require(decodedData.rows.size() == rows.size(), "Row count changed");
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            require(decodedData.rows[i].row.values == rows[i].values, "Row round trip failed");
        }
        require(encodeDataPage(decoded.header, schema, decodedData).bytes == data.bytes,
                "Slotted page reencoding changed bytes");
        require(encodeDataPage(decoded.header, schema, rows).bytes == data.bytes,
                "Header-taking data encoder changed bytes");

        ByteReader rawReader(data.bytes);
        SlotDecoder slots(rawReader);
        const Slot first = slots.decodeSlot(0);
        require(first.offset == PageHeaderLayout::Size &&
                    first.size == encodedRowSize(schema, rows[0]),
                "Slot directory was not copied from the writer buffer");
        require(rawReader.position() == 0, "Slot decoding moved the reader position");
        require(std::all_of(data.begin() + decoded.header.freeSpaceStart,
                            data.begin() + decoded.header.freeSpaceEnd,
                            [](std::byte byte) { return byte == std::byte{0}; }),
                "Free-space padding is not zeroed");

        decodedData.slots[0].set(SlotFlag::Deleted);
        decodedData.rows.erase(decodedData.rows.begin());
        const RawPage deleted = encodeDataPage(decoded.header, schema, decodedData);
        require(pageHash(deleted) == 2408608308031514950ULL, "Deleted-slot encoding changed");
        const Page restored = decodeDataPage(deleted, schema);
        const DataPage &restoredData = std::get<DataPage>(restored.data);
        require(restoredData.slots[0].has(SlotFlag::Deleted) && restoredData.rows.size() == 1 &&
                    restoredData.rows[0].slotIndex == 1 && restoredData.rows[0].row.values == rows[1].values,
                "Deleted-slot round trip failed");

        const RawPage empty = encodeDataPage(7, schema, std::vector<Row>{});
        require(pageHash(empty) == 5127982419751494579ULL, "Empty-page encoding changed");
        require(std::get<DataPage>(decodeDataPage(empty, schema).data).rows.empty(),
                "Empty data page did not decode");
    }
}

int main()
{
    testUnsignedEncoding();
    testStringsAndLargeBuffers();
    testWriterFailures();
    testReaderFailures();
    testEmptyAndRawBuffers();
    testPageCompatibility();
}
