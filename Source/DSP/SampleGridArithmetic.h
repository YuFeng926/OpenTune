#pragma once

#include <cstdint>
#include <limits>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#include <immintrin.h>
#endif

namespace OpenTune::SampleGrid {

/** Unsigned 128-bit value used for exact 64x64 products. */
struct U128
{
    uint64_t high{0};
    uint64_t low{0};
};

inline U128 multiplyU64(uint64_t lhs, uint64_t rhs) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    U128 result;
    result.low = _umul128(lhs, rhs, &result.high);
    return result;
#elif defined(__SIZEOF_INT128__)
    const unsigned __int128 product =
        static_cast<unsigned __int128>(lhs) * static_cast<unsigned __int128>(rhs);
    return { static_cast<uint64_t>(product >> 64), static_cast<uint64_t>(product) };
#else
    const uint64_t lhsLow = lhs & 0xffffffffu;
    const uint64_t lhsHigh = lhs >> 32;
    const uint64_t rhsLow = rhs & 0xffffffffu;
    const uint64_t rhsHigh = rhs >> 32;
    const uint64_t p0 = lhsLow * rhsLow;
    const uint64_t p1 = lhsLow * rhsHigh;
    const uint64_t p2 = lhsHigh * rhsLow;
    const uint64_t p3 = lhsHigh * rhsHigh;
    const uint64_t middle = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
    return { p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32),
             (p0 & 0xffffffffu) | (middle << 32) };
#endif
}

/** Divides a 128-bit value by a 64-bit divisor. The quotient must fit 64 bits. */
inline uint64_t divideU128(U128 value, uint64_t divisor, uint64_t& remainder) noexcept
{
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    return _udiv128(value.high, value.low, divisor, &remainder);
#elif defined(__SIZEOF_INT128__)
    const unsigned __int128 numerator =
        (static_cast<unsigned __int128>(value.high) << 64) | value.low;
    remainder = static_cast<uint64_t>(numerator % divisor);
    return static_cast<uint64_t>(numerator / divisor);
#else
    uint64_t quotient = 0;
    U128 current{0, 0};

    for (int bit = 127; bit >= 0; --bit)
    {
        const uint64_t incoming = bit >= 64
            ? ((value.high >> (bit - 64)) & 1u)
            : ((value.low >> bit) & 1u);

        current.high = (current.high << 1) | (current.low >> 63);
        current.low = (current.low << 1) | incoming;

        if (current.high != 0 || current.low >= divisor)
        {
            const uint64_t borrow = current.low < divisor ? 1u : 0u;
            current.low -= divisor;
            current.high -= borrow;

            if (bit < 64)
                quotient |= (uint64_t{1} << bit);
        }
    }

    remainder = current.low;
    return quotient;
#endif
}

inline int compareU128(U128 lhs, U128 rhs) noexcept
{
    if (lhs.high != rhs.high)
        return lhs.high < rhs.high ? -1 : 1;
    if (lhs.low != rhs.low)
        return lhs.low < rhs.low ? -1 : 1;
    return 0;
}

inline U128 incrementU128(U128 value) noexcept
{
    if (++value.low == 0)
        ++value.high;
    return value;
}

/** Exact 128-bit by 64-bit product; the full product must fit 128 bits. */
inline U128 multiplyU128(U128 value, uint64_t factor) noexcept
{
    U128 product = multiplyU64(value.low, factor);
    product.high += value.high * factor;
    return product;
}

/** Divides a 128-bit value by a 64-bit divisor, retaining a 128-bit quotient. */
inline U128 divideU128Full(U128 value, uint64_t divisor, uint64_t& remainder) noexcept
{
    U128 quotient{0, 0};

    if (value.high == 0)
    {
        quotient.low = divideU128(value, divisor, remainder);
        return quotient;
    }

    quotient.high = value.high / divisor;
    const uint64_t highRemainder = value.high % divisor;
    quotient.low = divideU128({ highRemainder, value.low }, divisor, remainder);
    return quotient;
}

/** Signed 128-bit sample coordinate stored as sign and magnitude. */
struct Wide
{
    bool negative{false};
    uint64_t high{0};
    uint64_t low{0};
};

inline Wide wideFromInt64(int64_t value) noexcept
{
    Wide result;

    if (value < 0)
    {
        result.negative = true;
        result.low = uint64_t{0} - static_cast<uint64_t>(value);
    }
    else
    {
        result.low = static_cast<uint64_t>(value);
    }

    return result;
}

inline int compareWide(const Wide& lhs, const Wide& rhs) noexcept
{
    if (lhs.negative != rhs.negative)
        return lhs.negative ? -1 : 1;

    const int order = compareU128({ lhs.high, lhs.low }, { rhs.high, rhs.low });
    return lhs.negative ? -order : order;
}

inline Wide addWide(const Wide& value, int64_t addend) noexcept
{
    const Wide other = wideFromInt64(addend);

    if (value.negative == other.negative)
    {
        U128 magnitude{ value.high, value.low };
        const U128 addition{ other.high, other.low };
        const uint64_t low = magnitude.low + addition.low;
        magnitude.high += addition.high + (low < magnitude.low ? 1 : 0);
        magnitude.low = low;
        return { value.negative, magnitude.high, magnitude.low };
    }

    const U128 lhs{ value.high, value.low };
    const U128 rhs{ other.high, other.low };
    const int order = compareU128(lhs, rhs);

    if (order == 0)
        return {};

    if (order > 0)
    {
        U128 difference;
        difference.low = lhs.low - rhs.low;
        difference.high = lhs.high - rhs.high - (lhs.low < rhs.low ? 1 : 0);
        return { value.negative, difference.high, difference.low };
    }

    U128 difference;
    difference.low = rhs.low - lhs.low;
    difference.high = rhs.high - lhs.high - (rhs.low < lhs.low ? 1 : 0);
    return { other.negative, difference.high, difference.low };
}

inline Wide scaleWide(const Wide& value, uint64_t factor) noexcept
{
    if (factor == 0 || (value.high == 0 && value.low == 0))
        return {};

    const U128 product = multiplyU128({ value.high, value.low }, factor);
    return { value.negative, product.high, product.low };
}

inline int64_t wideToInt64(const Wide& value) noexcept
{
    const uint64_t magnitude = value.low;

    if (!value.negative)
        return static_cast<int64_t>(magnitude);

    if (magnitude == (uint64_t{1} << 63))
        return std::numeric_limits<int64_t>::min();

    return -static_cast<int64_t>(magnitude);
}

/** Exact floor( value / divisor ) for a signed wide value. */
inline Wide floorDivideWide(const Wide& value, uint64_t divisor) noexcept
{
    uint64_t remainder = 0;
    const U128 quotient = divideU128Full({ value.high, value.low }, divisor, remainder);

    if (!value.negative)
        return { false, quotient.high, quotient.low };

    if (remainder == 0)
        return { true, quotient.high, quotient.low };

    const U128 rounded = incrementU128(quotient);
    return { true, rounded.high, rounded.low };
}

/** Exact ceil( value / divisor ) for a signed wide value. */
inline Wide ceilDivideWide(const Wide& value, uint64_t divisor) noexcept
{
    uint64_t remainder = 0;
    const U128 quotient = divideU128Full({ value.high, value.low }, divisor, remainder);

    if (value.negative)
        return { true, quotient.high, quotient.low };

    if (remainder == 0)
        return { false, quotient.high, quotient.low };

    const U128 rounded = incrementU128(quotient);
    return { false, rounded.high, rounded.low };
}

/**
 * Computes floor(( value * factor + addend ) / divisor), or the ceiling when
 * `roundUp` is true. `factor` is non-negative and `divisor` is positive.
 */
inline Wide multiplyDivideAddWide(const Wide& value, uint64_t factor, int64_t addend,
    uint64_t divisor, bool roundUp) noexcept
{
    const Wide numerator = addWide(scaleWide(value, factor), addend);
    return roundUp ? ceilDivideWide(numerator, divisor) : floorDivideWide(numerator, divisor);
}

inline Wide multiplyDivideFloorWide(const Wide& value, uint64_t factor,
    uint64_t divisor) noexcept
{
    return multiplyDivideAddWide(value, factor, 0, divisor, false);
}

} // namespace OpenTune::SampleGrid
