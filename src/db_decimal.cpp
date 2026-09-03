#include "db_decimal.h"

#include <algorithm>
#include <charconv>
#include <compare>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>

namespace
{
    std::uint64_t magnitude(std::int64_t value)
    {
        if (value >= 0)
        {
            return static_cast<std::uint64_t>(value);
        }

        return static_cast<std::uint64_t>(-(value + 1)) + 1;
    }

    DecimalValue normalizeDecimal(DecimalValue value)
    {
        while (value.scale != 0 && value.coefficient % 10 == 0)
        {
            value.coefficient /= 10;
            --value.scale;
        }

        return value;
    }

    std::int64_t checkedScale(
        std::int64_t value,
        std::uint32_t places)
    {
        for (std::uint32_t i = 0; i < places; ++i)
        {
            if (value > std::numeric_limits<std::int64_t>::max() / 10 ||
                value < std::numeric_limits<std::int64_t>::min() / 10)
            {
                throw std::overflow_error("Decimal coefficient overflow");
            }

            value *= 10;
        }

        return value;
    }

    std::int64_t checkedAdd(std::int64_t left, std::int64_t right)
    {
        if ((right > 0 &&
             left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 &&
             left < std::numeric_limits<std::int64_t>::min() - right))
        {
            throw std::overflow_error("Decimal coefficient overflow");
        }

        return left + right;
    }

    std::int64_t checkedMultiply(std::int64_t left, std::int64_t right)
    {
        if (left == 0 || right == 0)
        {
            return 0;
        }

        if ((left == -1 &&
             right == std::numeric_limits<std::int64_t>::min()) ||
            (right == -1 &&
             left == std::numeric_limits<std::int64_t>::min()))
        {
            throw std::overflow_error("Decimal coefficient overflow");
        }

        if (left > 0)
        {
            if ((right > 0 &&
                 left > std::numeric_limits<std::int64_t>::max() / right) ||
                (right < 0 &&
                 right < std::numeric_limits<std::int64_t>::min() / left))
            {
                throw std::overflow_error("Decimal coefficient overflow");
            }
        }
        else if ((right > 0 &&
                  left < std::numeric_limits<std::int64_t>::min() / right) ||
                 (right < 0 &&
                  left < std::numeric_limits<std::int64_t>::max() / right))
        {
            throw std::overflow_error("Decimal coefficient overflow");
        }

        return left * right;
    }

    std::string scaledMagnitude(
        const DecimalValue &value,
        std::uint32_t commonScale)
    {
        if (value.coefficient == 0)
        {
            return "0";
        }

        std::string digits = std::to_string(magnitude(value.coefficient));
        digits.append(commonScale - value.scale, '0');
        return digits;
    }

    std::strong_ordering compareMagnitudes(
        const DecimalValue &left,
        const DecimalValue &right)
    {
        const std::uint32_t commonScale =
            std::max(left.scale, right.scale);
        const std::string leftDigits = scaledMagnitude(left, commonScale);
        const std::string rightDigits = scaledMagnitude(right, commonScale);

        if (leftDigits.size() < rightDigits.size())
        {
            return std::strong_ordering::less;
        }
        if (leftDigits.size() > rightDigits.size())
        {
            return std::strong_ordering::greater;
        }

        if (leftDigits < rightDigits)
        {
            return std::strong_ordering::less;
        }
        if (leftDigits > rightDigits)
        {
            return std::strong_ordering::greater;
        }

        return std::strong_ordering::equal;
    }
}

DecimalValue parseDecimalLiteral(std::string_view text)
{
    if (text.empty())
    {
        throw std::runtime_error("Empty decimal literal");
    }

    bool negative = false;
    if (text.front() == '+' || text.front() == '-')
    {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }

    const std::size_t decimalPoint = text.find('.');
    if (decimalPoint == std::string_view::npos ||
        text.find('.', decimalPoint + 1) != std::string_view::npos)
    {
        throw std::runtime_error("Invalid decimal literal");
    }

    std::string integral{text.substr(0, decimalPoint)};
    std::string fractional{text.substr(decimalPoint + 1)};
    if (integral.empty() || fractional.empty())
    {
        throw std::runtime_error("Invalid decimal literal");
    }

    const auto isDigit = [](char character)
    {
        return character >= '0' && character <= '9';
    };
    if (!std::all_of(integral.begin(), integral.end(), isDigit) ||
        !std::all_of(fractional.begin(), fractional.end(), isDigit))
    {
        throw std::runtime_error("Invalid decimal literal");
    }

    while (!fractional.empty() && fractional.back() == '0')
    {
        fractional.pop_back();
    }
    if (fractional.size() > MAX_DECIMAL_SCALE)
    {
        throw std::overflow_error("Decimal scale exceeds 18 digits");
    }

    std::string digits = integral + fractional;
    const std::size_t firstNonZero = digits.find_first_not_of('0');
    if (firstNonZero == std::string::npos)
    {
        return DecimalValue{.coefficient = 0, .scale = 0};
    }
    digits.erase(0, firstNonZero);

    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        digits.data(),
        digits.data() + digits.size(),
        parsed);
    if (result.ec == std::errc::result_out_of_range ||
        result.ptr != digits.data() + digits.size())
    {
        throw std::overflow_error("Decimal coefficient exceeds 64 bits");
    }

    const std::uint64_t positiveLimit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t negativeLimit = positiveLimit + 1;
    if ((!negative && parsed > positiveLimit) ||
        (negative && parsed > negativeLimit))
    {
        throw std::overflow_error("Decimal coefficient exceeds int64 range");
    }

    std::int64_t coefficient;
    if (!negative)
    {
        coefficient = static_cast<std::int64_t>(parsed);
    }
    else if (parsed == negativeLimit)
    {
        coefficient = std::numeric_limits<std::int64_t>::min();
    }
    else
    {
        coefficient = -static_cast<std::int64_t>(parsed);
    }

    return normalizeDecimal(DecimalValue{
        .coefficient = coefficient,
        .scale = static_cast<std::uint32_t>(fractional.size())});
}

DecimalValue decimalFromInt64(std::int64_t value)
{
    return DecimalValue{.coefficient = value, .scale = 0};
}

std::optional<std::int64_t> decimalToInt64Exact(
    const DecimalValue &value)
{
    std::int64_t coefficient = value.coefficient;
    for (std::uint32_t i = 0; i < value.scale; ++i)
    {
        if (coefficient % 10 != 0)
        {
            return std::nullopt;
        }
        coefficient /= 10;
    }

    return coefficient;
}

std::float64_t decimalToFloat64(const DecimalValue &value)
{
    std::float64_t result =
        static_cast<std::float64_t>(value.coefficient);
    for (std::uint32_t i = 0; i < value.scale; ++i)
    {
        result /= static_cast<std::float64_t>(10);
    }
    return result;
}

std::string decimalToString(const DecimalValue &value)
{
    std::string digits = std::to_string(magnitude(value.coefficient));
    std::string result;

    if (value.coefficient < 0)
    {
        result.push_back('-');
    }

    if (value.scale == 0)
    {
        return result + digits;
    }

    if (digits.size() <= value.scale)
    {
        result += "0.";
        result.append(value.scale - digits.size(), '0');
        result += digits;
        return result;
    }

    const std::size_t decimalPoint = digits.size() - value.scale;
    result.append(digits, 0, decimalPoint);
    result.push_back('.');
    result.append(digits, decimalPoint, std::string::npos);
    return result;
}

DecimalValue negateDecimal(const DecimalValue &value)
{
    if (value.coefficient == std::numeric_limits<std::int64_t>::min())
    {
        throw std::overflow_error("Decimal coefficient overflow");
    }

    return DecimalValue{
        .coefficient = -value.coefficient,
        .scale = value.scale};
}

DecimalValue addDecimals(
    const DecimalValue &left,
    const DecimalValue &right)
{
    const std::uint32_t scale = std::max(left.scale, right.scale);
    return normalizeDecimal(DecimalValue{
        .coefficient = checkedAdd(
            checkedScale(left.coefficient, scale - left.scale),
            checkedScale(right.coefficient, scale - right.scale)),
        .scale = scale});
}

DecimalValue subtractDecimals(
    const DecimalValue &left,
    const DecimalValue &right)
{
    return addDecimals(left, negateDecimal(right));
}

DecimalValue multiplyDecimals(
    const DecimalValue &left,
    const DecimalValue &right)
{
    if (left.scale + right.scale > MAX_DECIMAL_SCALE)
    {
        throw std::overflow_error("Decimal scale exceeds 18 digits");
    }

    return normalizeDecimal(DecimalValue{
        .coefficient = checkedMultiply(
            left.coefficient,
            right.coefficient),
        .scale = left.scale + right.scale});
}

DecimalValue divideDecimals(
    const DecimalValue &left,
    const DecimalValue &right)
{
    if (right.coefficient == 0)
    {
        throw std::runtime_error("Division by zero in decimal expression");
    }

    for (std::uint32_t scale = 0; scale <= MAX_DECIMAL_SCALE; ++scale)
    {
        const std::int32_t exponent =
            static_cast<std::int32_t>(right.scale) +
            static_cast<std::int32_t>(scale) -
            static_cast<std::int32_t>(left.scale);

        try
        {
            std::int64_t numerator = left.coefficient;
            std::int64_t denominator = right.coefficient;
            if (exponent >= 0)
            {
                numerator = checkedScale(
                    numerator,
                    static_cast<std::uint32_t>(exponent));
            }
            else
            {
                denominator = checkedScale(
                    denominator,
                    static_cast<std::uint32_t>(-exponent));
            }

            if (numerator == std::numeric_limits<std::int64_t>::min() &&
                denominator == -1)
            {
                throw std::overflow_error("Decimal coefficient overflow");
            }

            if (numerator % denominator == 0)
            {
                return normalizeDecimal(DecimalValue{
                    .coefficient = numerator / denominator,
                    .scale = scale});
            }
        }
        catch (const std::overflow_error &)
        {
        }
    }

    throw std::runtime_error(
        "Decimal division has no exact result within 18 fractional digits");
}

bool operator==(const DecimalValue &left, const DecimalValue &right)
{
    return (left <=> right) == std::strong_ordering::equal;
}

std::strong_ordering operator<=>(
    const DecimalValue &left,
    const DecimalValue &right)
{
    if (left.coefficient < 0 && right.coefficient >= 0)
    {
        return std::strong_ordering::less;
    }
    if (left.coefficient >= 0 && right.coefficient < 0)
    {
        return std::strong_ordering::greater;
    }

    const std::strong_ordering magnitudeOrder =
        compareMagnitudes(left, right);
    if (left.coefficient >= 0)
    {
        return magnitudeOrder;
    }
    if (magnitudeOrder == std::strong_ordering::less)
    {
        return std::strong_ordering::greater;
    }
    if (magnitudeOrder == std::strong_ordering::greater)
    {
        return std::strong_ordering::less;
    }
    return std::strong_ordering::equal;
}

std::ostream &operator<<(std::ostream &stream, const DecimalValue &value)
{
    return stream << decimalToString(value);
}
