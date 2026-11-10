#pragma once

#include "db_storage.h"

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <type_traits>
#include <concepts>

Page makeEmptyDataPage(std::uint32_t pageId);

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

Page makeHeaderPage(
    const CreateIndexPayload &payload);
Page makeHeaderPage(
    const CreateTablePayload &payload);
