#pragma once

#include "db_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

constexpr std::uint32_t MAX_DECIMAL_SCALE = 18;

DecimalValue parseDecimalLiteral(std::string_view text);
DecimalValue decimalFromInt64(std::int64_t value);
DecimalValue decimalAbs(const DecimalValue &value);
DecimalValue decimalRound(const DecimalValue &value, std::uint32_t scale);

std::optional<std::int64_t> decimalToInt64Exact(
    const DecimalValue &value);
std::float64_t decimalToFloat64(const DecimalValue &value);
std::string decimalToString(const DecimalValue &value);

DecimalValue negateDecimal(const DecimalValue &value);
DecimalValue addDecimals(
    const DecimalValue &left,
    const DecimalValue &right);
DecimalValue subtractDecimals(
    const DecimalValue &left,
    const DecimalValue &right);
DecimalValue multiplyDecimals(
    const DecimalValue &left,
    const DecimalValue &right);
DecimalValue divideDecimals(
    const DecimalValue &left,
    const DecimalValue &right);
