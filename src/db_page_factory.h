#pragma once

#include "db_storage.h"

#include <cstdint>
#include <string>
#include <vector>

Page makeEmptyDataPage(std::uint32_t pageId);

Page makeHeaderPage(
    const std::string &name,
    const std::string &magic,
    const std::vector<Column> &columns,
    const std::vector<Constraint> &constraints);
