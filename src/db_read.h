#pragma once

#include "db_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

class PageDecoder
{
public:
    explicit PageDecoder(const RawPage &buffer);

    std::size_t position() const;
    void seek(std::size_t newPos);

    template <typename T>
    T decodeUnsigned()
    {
        static_assert(std::is_unsigned_v<T>, "T must be an unsigned integer type");
        ensureAvailable(sizeof(T));

        T value = 0;

        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            std::uint8_t byte = std::to_integer<std::uint8_t>(buffer[pos + i]);
            value |= static_cast<T>(byte) << (i * 8);
        }

        pos += sizeof(T);
        return value;
    }

    template <typename T>
    T decodeUnsignedAt(std::size_t offset)
    {
        std::size_t saved = position();
        seek(offset);

        T value = decodeUnsigned<T>();

        seek(saved);
        return value;
    }

    std::string decodeString();
    void decodeBytes(void *out, std::size_t size);
    std::vector<std::byte> decodeBytes(std::size_t size);
    std::vector<std::byte> decodeBytesAt(std::size_t offset, std::size_t size);

private:
    const RawPage &buffer;
    std::size_t pos = 0;

    void ensureAvailable(std::size_t size) const;
};

class PageHeaderDecoder
{
public:
    explicit PageHeaderDecoder(PageDecoder &decoder);

    PageHeader decode();

private:
    PageDecoder &decoder;
};

class HeaderPageDecoder
{
public:
    explicit HeaderPageDecoder(PageDecoder &decoder);

    Page decode();

private:
    PageDecoder &decoder;
    PageHeaderDecoder pageHeaderDecoder;

    Column decodeColumn();
    ColumnStorage decodeColumnStorage();
};

class ValueDeserializer
{
public:
    static Value decodeFixed(
        PageDecoder &decoder,
        std::size_t absoluteOffset,
        DataType type);

    static Value decodeVariable(
        PageDecoder &decoder,
        std::size_t absoluteOffset,
        std::uint32_t length,
        DataType type);
};

class RowDecoder
{
public:
    RowDecoder(
        PageDecoder &decoder,
        const HeaderPage &headerPage,
        std::size_t rowStart);

    Row decodeRow();

private:
    PageDecoder &decoder;
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

    explicit SlotDecoder(PageDecoder &decoder);

    Slot decodeSlot(std::uint16_t slotIndex) const;
    std::vector<Slot> decodeSlots(std::uint16_t slotCount) const;
    std::size_t slotOffset(std::uint16_t slotIndex) const;

private:
    PageDecoder &decoder;
};

class DataPageDecoder
{
public:
    DataPageDecoder(PageDecoder &decoder, const HeaderPage &headerPage);

    Page decode();

private:
    PageDecoder &decoder;
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
