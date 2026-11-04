// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContentHandles.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace OpenTune {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
constexpr std::size_t kInstanceShortLength = 8;

char prefixOf(ControlHandleKind kind)
{
    return kind == ControlHandleKind::Content ? 'c' : 'p';
}

bool isLowerHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/** A decimal number as the pattern (0|[1-9][0-9]*) allows it. Sets overflow when it exceeds 64 bits. */
bool parseDecimal(std::string_view text, std::uint64_t& value, bool& overflow)
{
    if (text.empty() || (text.size() > 1 && text[0] == '0'))
        return false;
    value = 0;
    overflow = false;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return false;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            overflow = true;
        else
            value = value * 10 + digit;
    }
    return true;
}

std::string hex16(std::uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[value & 0xf];
        value >>= 4;
    }
    return out;
}

/** A token as ControlTokenHasher writes it: one letter and 16 lowercase hexadecimal digits. */
bool isHashToken(std::string_view text)
{
    if (text.size() != 17 || !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z')))
        return false;
    for (std::size_t i = 1; i < text.size(); ++i)
        if (!isLowerHexDigit(text[i]))
            return false;
    return true;
}

} // namespace

std::optional<ControlHandle> controlParseHandle(ControlHandleKind kind, std::string_view text)
{
    // <prefix>:<8 hex>:<epoch>:<object>
    if (text.size() < 2 + kInstanceShortLength + 4 || text[0] != prefixOf(kind) || text[1] != ':')
        return std::nullopt;
    const std::string_view instance = text.substr(2, kInstanceShortLength);
    for (const char c : instance)
        if (!isLowerHexDigit(c))
            return std::nullopt;
    if (text[2 + kInstanceShortLength] != ':')
        return std::nullopt;
    const std::string_view rest = text.substr(3 + kInstanceShortLength);
    const auto colon = rest.find(':');
    if (colon == std::string_view::npos)
        return std::nullopt;
    ControlHandle handle;
    handle.kind = kind;
    handle.instanceShort = std::string(instance);
    if (!parseDecimal(rest.substr(0, colon), handle.epoch, handle.epochOverflow)
        || !parseDecimal(rest.substr(colon + 1), handle.object, handle.objectOverflow))
        return std::nullopt;
    return handle;
}

std::string controlInstanceShort(std::string_view instanceId)
{
    return std::string(instanceId.substr(0, kInstanceShortLength));
}

std::string controlFormatHandle(ControlHandleKind kind, std::string_view instanceShort, std::uint64_t epoch,
                                std::uint64_t object)
{
    std::string out;
    out += prefixOf(kind);
    out += ':';
    out += instanceShort;
    out += ':';
    out += std::to_string(epoch);
    out += ':';
    out += std::to_string(object);
    return out;
}

ControlTokenHasher::ControlTokenHasher(char prefix) : prefix_(prefix), hash_(kFnvOffsetBasis) {}

void ControlTokenHasher::addBytes(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash_ ^= bytes[i];
        hash_ *= kFnvPrime;
    }
}

ControlTokenHasher& ControlTokenHasher::add(std::uint64_t value)
{
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i)
        bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
    addBytes(bytes, sizeof(bytes));
    return *this;
}

ControlTokenHasher& ControlTokenHasher::add(std::int64_t value)
{
    return add(static_cast<std::uint64_t>(value));
}

ControlTokenHasher& ControlTokenHasher::add(double value)
{
    if (value == 0.0)
        value = 0.0; // -0.0 and 0.0 are the same value
    if (std::isnan(value))
        value = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "binary64 expected");
    std::memcpy(&bits, &value, sizeof(bits));
    return add(bits);
}

ControlTokenHasher& ControlTokenHasher::add(std::string_view text)
{
    add(static_cast<std::uint64_t>(text.size()));
    addBytes(text.data(), text.size());
    return *this;
}

std::string ControlTokenHasher::token() const
{
    return std::string(1, prefix_) + hex16(hash_);
}

std::string controlFormatCursor(const ControlCursor& cursor)
{
    return std::string(1, cursor.kind) + "1." + std::to_string(cursor.offset) + "." + cursor.paramsHash + "."
           + cursor.tokensHash;
}

std::optional<ControlCursor> controlParseCursor(char kind, std::string_view text)
{
    if (text.size() < 3 || text[0] != kind || text[1] != '1' || text[2] != '.')
        return std::nullopt;
    const std::string_view rest = text.substr(3);
    const auto first = rest.find('.');
    if (first == std::string_view::npos)
        return std::nullopt;
    const auto second = rest.find('.', first + 1);
    if (second == std::string_view::npos)
        return std::nullopt;
    ControlCursor cursor;
    cursor.kind = kind;
    bool overflow = false;
    if (!parseDecimal(rest.substr(0, first), cursor.offset, overflow) || overflow)
        return std::nullopt;
    cursor.paramsHash = std::string(rest.substr(first + 1, second - first - 1));
    cursor.tokensHash = std::string(rest.substr(second + 1));
    if (!isHashToken(cursor.paramsHash) || !isHashToken(cursor.tokensHash))
        return std::nullopt;
    return cursor;
}

} // namespace OpenTune
