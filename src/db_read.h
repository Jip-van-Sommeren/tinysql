#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <filesystem>
#include <fstream>
#include <format>
#include <cmath>
#include "db_storage.h"

template <typename T, typename Buffer>
T readUnsigned(const Buffer &buffer, size_t &offset);
template <typename T, typename Buffer>
T readSigned(const Buffer &buffer, size_t &offset);
template <typename Buffer>
uint8_t readUInt8(const Buffer &buffer, size_t &offset);
template <typename Buffer>
uint16_t readUInt16(const Buffer &buffer, size_t &offset);
template <typename Buffer>
uint32_t readUInt32(const Buffer &buffer, size_t &offset);
template <typename Buffer>
uint64_t readUInt64(const Buffer &buffer, size_t &offset);
template <typename Buffer>
int32_t readInt32(const Buffer &buffer, size_t &offset);

template <typename Buffer>
std::string readString(const Buffer &buffer, size_t &offset);
template <typename Buffer>
Value readValue(const Buffer &buffer, size_t &offset);

template <typename Buffer>
Row readRowAt(const Buffer &buffer, const Slot &slot);

template <typename Buffer>
Column readColumn(const Buffer &buffer, size_t &offset);
RawPage readPageFromFile(
    const std::filesystem::path &tablePath,
    uint32_t pageId);

template <typename Reader>
auto decodePage(const RawPage &buffer, Reader reader)
{
    return reader(buffer);
}

Page decodeHeaderPage(const RawPage &page);

Slot decodeSlot(const RawPage &page, size_t &offset);
Page decodeDataPage(const RawPage &page);
bool isValidPageType(uint8_t value);

PageHeader decodePageHeader(const RawPage &page, size_t &offset);
