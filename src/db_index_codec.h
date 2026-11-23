#pragma once

#include "db_storage.h"
#include <span>

// NULL sorts first. Equal keys are ordered by their original table RowId.
int compareIndexKeys(const Row &left, const Row &right);

int comparePartialIndexKeys(const Row &key, const Row &target);
bool indexEntryLess(const LeafIndexEntry &left, const LeafIndexEntry &right);
bool indexKeyHasNull(const Row &key);
bool indexKeyHasNaN(const Row &key);

LeafIndexEntry decodeLeafIndexEntry(std::span<const std::byte> bytes, const RowSchema &schema);
InternalIndexEntry decodeInternalIndexEntry(std::span<const std::byte> bytes, const RowSchema &schema);
RawPage encodeIndexPage(const Page &page, const RowSchema &schema);
Page decodeIndexPage(const RawPage &raw, const RowSchema &schema);
