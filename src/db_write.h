#pragma once

#include "db_storage.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

class PageWriter
{
public:
    explicit PageWriter(RawPage &buffer);

    std::size_t position() const;
    void seek(std::size_t newPos);

    template <typename T>
    void writeUnsigned(T value)
    {
        static_assert(std::is_unsigned_v<T>, "T must be an unsigned integer type");
        ensureCapacity(sizeof(T));

        for (std::size_t shift = 0; shift < sizeof(T) * 8; shift += 8)
        {
            buffer[pos++] = static_cast<std::byte>((value >> shift) & 0xFF);
        }
    }

    template <typename T>
    void writeUnsignedAt(std::size_t offset, T value)
    {
        std::size_t saved = pos;
        seek(offset);
        writeUnsigned<T>(value);
        seek(saved);
    }

    void writeBytes(const void *data, std::size_t size);
    void writeBytesAt(std::size_t offset, const void *data, std::size_t size);
    void writeString(const std::string &value);

private:
    RawPage &buffer;
    std::size_t pos = 0;

    void ensureCapacity(std::size_t size) const;
};

class ValueSerializer
{
public:
    static void writeFixed(
        PageWriter &writer,
        std::size_t absoluteOffset,
        DataType type,
        const Value &value);

    static std::vector<std::byte> serializeVariable(
        DataType type,
        const Value &value);
};

class RowWriter
{
public:
    RowWriter(PageWriter &writer, const HeaderPage &headerPage);

    void writeRow(const std::vector<Value> &values);

private:
    PageWriter &writer;
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

    explicit SlotWriter(PageWriter &writer);

    void writeSlot(std::uint16_t slotIndex, const Slot &slot);
    void writeSlot(
        std::uint16_t slotIndex,
        std::uint16_t rowOffset,
        std::uint16_t rowSize,
        std::uint16_t flags = 0);
    void markDeleted(std::uint16_t slotIndex, Slot slot);
    std::size_t slotOffset(std::uint16_t slotIndex) const;

private:
    PageWriter &writer;
};

class PageHeaderWriter
{
public:
    explicit PageHeaderWriter(PageWriter &writer);

    void write(const PageHeader &header);
    void initializeDataPage(std::uint32_t pageId);
    void setSlotCount(std::uint16_t slotCount);
    void setFreeSpaceStart(std::uint16_t freeSpaceStart);
    void setFreeSpaceEnd(std::uint16_t freeSpaceEnd);
    void setNextPageId(std::uint32_t nextPageId);

private:
    PageWriter &writer;
};

class HeaderPageWriter
{
public:
    explicit HeaderPageWriter(PageWriter &writer);

    void write(const HeaderPage &header);

private:
    PageWriter &writer;

    void writeColumn(const Column &column);
    void writeColumnStorage(const ColumnStorage &storage);
};

class DataPageWriter
{
public:
    DataPageWriter(PageWriter &writer, const HeaderPage &tableHeader);

    void write(std::uint32_t pageId, const std::vector<Row> &rows);
    void write(const PageHeader &pageHeader, const std::vector<Row> &rows);

private:
    PageWriter &writer;
    PageHeaderWriter headerWriter;
    SlotWriter slotWriter;
    const HeaderPage &tableHeader;

    std::size_t computeRowSize(const std::vector<Value> &values) const;
};

RawPage encodeHeaderPage(const PageHeader &pageHeader, const HeaderPage &headerPage);
RawPage encodeDataPage(
    const PageHeader &pageHeader,
    const HeaderPage &tableHeader,
    const std::vector<Row> &rows);
RawPage encodeDataPage(
    std::uint32_t pageId,
    const HeaderPage &tableHeader,
    const std::vector<Row> &rows);
RawPage encodePage(const Page &page);

std::uint16_t calculateFreeSpaceStart(const PageHeader &pageHeader);
std::size_t encodedSlotSize();
std::size_t encodedRowSize(const HeaderPage &tableHeader, const Row &row);

// Compatibility helpers used by the current storage implementation.
void writePageHeader(std::vector<std::byte> &buffer, const PageHeader &header);
void writeHeaderPage(std::vector<std::byte> &buffer, const HeaderPage &header);
void writeSlot(std::vector<std::byte> &buffer, const Slot &slot);
std::size_t encodedRowSize(const Row &row);
std::vector<std::byte> encodeRowPayload(const Row &row);
