#pragma once

#include "db_storage.h"

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <type_traits>

Page makeEmptyDataPage(std::uint32_t pageId);

Page makeHeaderPage(
    const std::string &name,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<BoundConstraint> &constraints);

template <typename T>
Page makeEmptyPage(std::uint32_t pageId)
{
    T dataPage{};
    std::size_t freeSpaceStart = PageHeaderLayout::Size;

    if constexpr (std::is_same_v<T, IndexPage>)
    {
        freeSpaceStart += dataPage.header.serializedSize();
    }

    PageHeader pageHeader{
        .pageId = pageId,
        .pageType = T::type(),
        .slotCount = 0,
        .freeSpaceStart = static_cast<std::uint16_t>(freeSpaceStart),
        .freeSpaceEnd = static_cast<std::uint16_t>(PAGE_SIZE),
        .nextPageId = 0};

    return Page{
        .header = pageHeader,
        .data = std::move(dataPage)};
}

template <typename T>
Page makeHeaderPage(
    const std::string &magic,
    const T &payload)
{
    PageHeader pageHeader{
        .pageId = 0,
        .pageType = PageType::HeaderPage,
        .slotCount = 0,
        .freeSpaceStart = 0,
        .freeSpaceEnd = static_cast<std::uint16_t>(PAGE_SIZE),
        .nextPageId = 1};

    HeaderPage tableHeader{
        .magic = magic,
        .version = 1,
        .pageSize = PAGE_SIZE,
        .tableName = name,
        .columns = buildStoredColumns(columns),
        .constraints = constraints,
        .totalRowCount = 0,
        .firstDataPageId = 1,
        .lastDataPageId = 1,
        .nextUnusedPageId = 2};

    pageHeader.freeSpaceStart =
        calculateHeaderFreeSpaceStart(pageHeader, tableHeader);

    return Page{
        .header = pageHeader,
        .data = std::move(tableHeader)};
}
