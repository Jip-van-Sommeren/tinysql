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
// magic = "MYDB"
// version = 1
// page_size = 4096
// table_name = "users"
// column_count = 2

// 4D 59 44 42      // "MYDB"
// 01 00 00 00      // version = 1
// 00 10 00 00      // page_size = 4096
// 05 00 00 00      // table name length = 5
// 75 73 65 72 73   // "users"
// 02 00 00 00      // column count = 2

template <typename T>
void writeUnsigned(std::vector<std::byte> &buffer, T value);

void writeUInt8(std::vector<std::byte> &buffer, uint8_t value);

void writeUInt16(std::vector<std::byte> &buffer, uint16_t value);

void writeUInt32(std::vector<std::byte> &buffer, uint32_t value);
void writeUInt64(std::vector<std::byte> &buffer, uint64_t value);
void writeInt8(std::vector<std::byte> &buffer, int8_t value);

void writeInt16(std::vector<std::byte> &buffer, int16_t value);
void writeInt32(std::vector<std::byte> &buffer, int32_t value);
void writeInt64(std::vector<std::byte> &buffer, int64_t value);
void writeString(std::vector<std::byte> &buffer, const std::string &value);
void writeValue(std::vector<std::byte> &buffer, const Value &value);

void writeRow(std::vector<std::byte> &buffer, const Row &row);
void writePageHeader(std::vector<std::byte> &buffer, const PageHeader &header);
void writeSlot(std::vector<std::byte> &buffer, const Slot &slot);

void writeDataPage(std::vector<std::byte> &buffer, const DataPage &dataPage);
void writeColumn(std::vector<std::byte> &buffer, const Column &column);

void writeHeaderPage(std::vector<std::byte> &buffer, const HeaderPage &header);

RawPage encodePage(const Page &page);
uint16_t calculateFreeSpaceStart(const PageHeader &pageHeader);

size_t valueEncodedSize(const Value &value);
size_t rowPayloadSize(const Row &row);

size_t encodedRowSize(const Row &row);
size_t encodedPageHeaderSize();
size_t encodedSlotSize();

std::vector<std::byte> encodeRowPayload(const Row &row);
