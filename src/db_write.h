#pragma once

#include "db_storage.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

// Owns a fixed-size, zero-initialized buffer; writes never resize it.
class ByteWriter
{
public:
    explicit ByteWriter(std::size_t size)
        : buffer(size) {}

    std::size_t position() const noexcept { return pos; }
    std::size_t size() const noexcept { return buffer.size(); }

    const std::vector<std::byte> &bytes() const noexcept
    {
        return buffer;
    }

    void seek(std::size_t newPos);

    template <typename T>
    void writeUnsigned(T value)
    {
        writeUnsignedAt(pos, value);
        pos += sizeof(T);
    }

    // Offset-based operations leave the sequential position unchanged.
    template <typename T>
    void writeUnsignedAt(std::size_t offset, T value)
    {
        static_assert(CHAR_BIT == 8, "This format requires 8-bit bytes");
        static_assert(
            std::is_integral_v<T> && std::is_unsigned_v<T> &&
                !std::is_same_v<std::remove_cv_t<T>, bool>,
            "T must be an unsigned integer type other than bool");

        ensureRange(offset, sizeof(T));
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            buffer[offset + i] =
                static_cast<std::byte>((value >> (i * 8)) & 0xFFu);
        }
    }

    void writeBytes(const void *data, std::size_t count);
    void writeBytesAt(std::size_t offset, const void *data, std::size_t count);
    // A little-endian uint32_t byte length followed by the string bytes.
    void writeString(const std::string &value);

private:
    std::vector<std::byte> buffer;
    std::size_t pos = 0;

    void ensureRange(std::size_t offset, std::size_t count) const;
};

class ValueSerializer
{
public:
    static std::vector<std::byte> serializeValue(
        DataType type,
        const Value &value);
};

class RowWriter
{
public:
    RowWriter(ByteWriter &writer, const HeaderPage &headerPage);

    void writeRow(const std::vector<Value> &values);
    static std::size_t computeSerializedRowSize(
        const HeaderPage &tableHeader,
        const std::vector<Value> &values);

private:
    ByteWriter &writer;
    const HeaderPage &headerPage;

    std::size_t rowStart = 0;
    std::size_t nullBitmapSizeBytes = 0;
    std::size_t fixedAreaSize = 0;
    std::size_t varDirSize = 0;
    std::size_t fixedAreaStartOffset = 0;
    std::size_t varDirStartOffset = 0;
    std::size_t varDataStartOffset = 0;
    std::size_t varDataPos = 0;

    static constexpr std::size_t VarEntrySize = 8;

    static void setBitmapBit(std::vector<std::uint8_t> &bitmap, std::size_t bitIndex);
    void prepareLayout();
    void writeFixedValues(const std::vector<Value> &values);
    void writeFixedValue(
        const Column &column,
        const FixedColumnStorage &fixed,
        const Value &value);
    void writeVariableValues(const std::vector<Value> &values);
    void writeVariableValue(
        const Column &column,
        const VarColumnStorage &var,
        const Value &value);
};

class BitmapWriter
{
public:
    explicit BitmapWriter(std::span<std::uint8_t> bytes);

    void set(std::size_t bitIndex);
    void clear(std::size_t bitIndex);
    bool get(std::size_t bitIndex) const;

private:
    std::span<std::uint8_t> bytes;
};

class SlotWriter
{
public:
    static constexpr std::size_t SlotSize = 6;

    explicit SlotWriter(ByteWriter &writer);

    void writeSlot(std::uint16_t slotIndex, const Slot &slot);
    void writeSlot(
        std::uint16_t slotIndex,
        std::uint16_t rowOffset,
        std::uint16_t rowSize,
        std::uint16_t flags = 0);
    void markDeleted(std::uint16_t slotIndex, Slot slot);
    std::size_t slotOffset(std::uint16_t slotIndex) const;

private:
    ByteWriter &writer;
};

class PageHeaderWriter
{
public:
    explicit PageHeaderWriter(ByteWriter &writer);

    void write(const PageHeader &header);
    void initializeDataPage(std::uint32_t pageId);
    void setSlotCount(std::uint16_t slotCount);
    void setFreeSpaceStart(std::uint16_t freeSpaceStart);
    void setFreeSpaceEnd(std::uint16_t freeSpaceEnd);
    void setNextPageId(std::uint32_t nextPageId);

private:
    ByteWriter &writer;
};

class HeaderPageWriter
{
public:
    explicit HeaderPageWriter(ByteWriter &writer);

    void write(const HeaderPage &header);

private:
    ByteWriter &writer;

    void writeColumn(const Column &column);
    void writeConstraint(const Constraint &constraint);
    void writeColumnStorage(const ColumnStorage &storage);
};

class DataPageWriter
{
public:
    DataPageWriter(ByteWriter &writer, const HeaderPage &tableHeader);

    void write(std::uint32_t pageId, const std::vector<Row> &rows);
    void write(const PageHeader &pageHeader, const std::vector<Row> &rows);

private:
    ByteWriter &writer;
    PageHeaderWriter headerWriter;
    SlotWriter slotWriter;
    const HeaderPage &tableHeader;
};

class ExpressionSerializer
{
public:
    static void serialize(
        const BoundExpr &expression,
        ByteWriter &writer);
};

RawPage encodeHeaderPage(const PageHeader &pageHeader, const HeaderPage &headerPage);
RawPage encodeDataPage(
    const PageHeader &pageHeader,
    const HeaderPage &tableHeader,
    const std::vector<Row> &rows);
RawPage encodeDataPage(
    const PageHeader &pageHeader,
    const HeaderPage &tableHeader,
    const DataPage &dataPage);
RawPage encodeDataPage(
    std::uint32_t pageId,
    const HeaderPage &tableHeader,
    const std::vector<Row> &rows);
RawPage encodePage(const Page &page);

std::size_t encodedSlotSize();
std::size_t encodedRowSize(const HeaderPage &tableHeader, const Row &row);

void writePageToFile(
    const std::filesystem::path &path,
    std::uint32_t pageId,
    const RawPage &page);
