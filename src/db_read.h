#pragma once

#include "db_storage.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

// Borrows its buffer: the backing storage must stay alive and must not
// be reallocated while the reader is in use.
class ByteReader
{
public:
    explicit ByteReader(std::span<const std::byte> buffer)
        : buffer(buffer) {}

    std::size_t position() const noexcept { return pos; }
    std::size_t size() const noexcept { return buffer.size(); }
    void seek(std::size_t newPos);

    template <typename T>
    T readUnsigned()
    {
        T value = readUnsignedAt<T>(pos);
        pos += sizeof(T);
        return value;
    }

    // Offset-based operations leave the sequential position unchanged.
    template <typename T>
    T readUnsignedAt(std::size_t offset) const
    {
        static_assert(CHAR_BIT == 8, "This format requires 8-bit bytes");
        static_assert(
            std::is_integral_v<T> && std::is_unsigned_v<T> &&
                !std::is_same_v<std::remove_cv_t<T>, bool>,
            "T must be an unsigned integer type other than bool");

        ensureRange(offset, sizeof(T));
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            const auto byte = std::to_integer<std::uint8_t>(buffer[offset + i]);
            value |= static_cast<T>(byte) << (i * 8);
        }
        return value;
    }

    // A little-endian uint32_t byte length followed by the string bytes.
    std::string readString();
    void readBytes(void *out, std::size_t count);
    std::vector<std::byte> readBytes(std::size_t count);
    std::vector<std::byte> readBytesAt(
        std::size_t offset, std::size_t count) const;

private:
    std::span<const std::byte> buffer;
    std::size_t pos = 0;

    void ensureRange(std::size_t offset, std::size_t count) const;
};

class PageHeaderDecoder
{
public:
    explicit PageHeaderDecoder(ByteReader &decoder);

    PageHeader decode();

private:
    ByteReader &decoder;
};

class HeaderPageDecoder
{
public:
    explicit HeaderPageDecoder(ByteReader &decoder);

    Page decode();

private:
    ByteReader &decoder;
    PageHeaderDecoder pageHeaderDecoder;

    Column decodeColumn();
    ColumnStorage decodeColumnStorage();
    Constraint decodeConstraint();
    std::unique_ptr<BoundExpr> decodeExpression();
    std::vector<ColumnId> decodeColumnIds();
};

class ValueDeserializer
{
public:
    static Value decodeFixed(
        ByteReader &decoder,
        std::size_t absoluteOffset,
        DataType type);

    static Value decodeVariable(
        ByteReader &decoder,
        std::size_t absoluteOffset,
        std::uint32_t length,
        DataType type);
};

class RowDecoder
{
public:
    RowDecoder(
        ByteReader &decoder,
        const HeaderPage &headerPage,
        std::size_t rowStart);

    Row decodeRow();

private:
    ByteReader &decoder;
    const HeaderPage &headerPage;
    std::size_t rowStart;

    std::size_t nullBitmapSizeBytes = 0;
    std::size_t fixedAreaSize = 0;
    std::size_t varDirSize = 0;
    std::size_t fixedAreaStartOffset = 0;
    std::size_t varDirStartOffset = 0;
    std::size_t varDataStartOffset = 0;

    static constexpr std::size_t VarEntrySize = 8;

    void prepareLayout();
    bool isNull(std::size_t columnIndex);
    Value decodeFixedValue(
        const Column &column,
        const FixedColumnStorage &fixed);
    Value decodeVariableValue(
        const Column &column,
        const VarColumnStorage &var);
};

class SlotDecoder
{
public:
    static constexpr std::size_t SlotSize = 6;

    explicit SlotDecoder(ByteReader &decoder);

    Slot decodeSlot(std::uint16_t slotIndex) const;
    std::vector<Slot> decodeSlots(std::uint16_t slotCount) const;
    std::size_t slotOffset(std::uint16_t slotIndex) const;

private:
    ByteReader &decoder;
};

class DataPageDecoder
{
public:
    DataPageDecoder(ByteReader &decoder, const HeaderPage &headerPage);

    Page decode();

private:
    ByteReader &decoder;
    PageHeaderDecoder headerDecoder;
    SlotDecoder slotDecoder;
    const HeaderPage &headerPage;
};

RawPage readPageFromFile(
    const std::filesystem::path &tablePath,
    std::uint32_t pageId);

template <typename Reader>
auto decodePage(const RawPage &buffer, Reader reader)
{
    return reader(buffer);
}

bool isValidPageType(std::uint8_t value);
PageHeader decodePageHeader(const RawPage &page);
Page decodeHeaderPage(const RawPage &page);
Page decodeDataPage(const RawPage &page, const HeaderPage &headerPage);
