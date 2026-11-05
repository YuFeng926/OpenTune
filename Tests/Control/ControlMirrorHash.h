// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Locating and hashing the text of mirrored upstream code, for ControlMirrorDriftTests. Standard library
// only, so that the test needs neither JUCE nor the processor.
//
// Two normalisations, both on the file as read (UTF-8 BOM removed, CRLF and CR turned into LF):
//
//   fn-v1    The text of one function: from the line that holds the qualified name followed by '(' to the
//            line of the '}' that closes the function body. Braces, parentheses and quotes inside
//            comments, string literals (raw ones too) and character literals are skipped.
//   stmt-v1  The text of one declaration: from the line that holds the given text (outside comments and
//            string literals) to the first ';' outside brackets, comments and literals.
//
// Both then do the same to the text: every line loses its leading and trailing white space, empty lines
// are dropped, runs of blanks and tabs inside a line become one space, the lines are joined with LF (no
// LF at the end), and the sha256 of the UTF-8 bytes is written as 64 lower-case hexadecimal digits.
// Comments are kept: a change of a comment shows up as a change and is reviewed like one. A change of
// formatting only (indentation, line breaks between lines, blank lines, line endings, trailing blanks) does
// not.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenTune {
namespace ControlMirror {

/** sha256 of the bytes, as 64 lower-case hexadecimal digits. */
inline std::string sha256Hex(std::string_view data)
{
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string message(data);
    const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8u;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56)
        message.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(static_cast<char>((bitLength >> shift) & 0xffu));

    const auto rotr = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); };
    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto byteAt = [&](std::size_t n) {
                return static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4 + n]));
            };
            w[i] = (byteAt(0) << 24) | (byteAt(1) << 16) | (byteAt(2) << 8) | byteAt(3);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t t2 = s0 + maj;
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + t1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t1 + t2;
        }
        for (std::size_t i = 0; i < 8; ++i)
            h[i] += v[i];
    }
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint32_t word : h) {
        for (int shift = 28; shift >= 0; shift -= 4)
            out.push_back(kDigits[(word >> shift) & 0xfu]);
    }
    return out;
}

/** The text as the normalisation reads it: no UTF-8 BOM, LF line endings. */
inline std::string normalizeSource(const std::string& raw)
{
    std::size_t begin = 0;
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB
        && static_cast<unsigned char>(raw[2]) == 0xBF)
        begin = 3;
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = begin; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < raw.size() && raw[i + 1] == '\n')
                ++i;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

namespace detail {

inline bool isIdent(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

inline bool isHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

inline bool isAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/** Index just after the comment, string literal (also a raw one) or character literal that starts at i;
    i when none starts there. */
inline std::size_t skipTrivia(const std::string& text, std::size_t i)
{
    const char c = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (c == '/' && next == '/') {
        const auto end = text.find('\n', i);
        return end == std::string::npos ? text.size() : end;
    }
    if (c == '/' && next == '*') {
        const auto end = text.find("*/", i + 2);
        return end == std::string::npos ? text.size() : end + 2;
    }
    if (c == '"') {
        if (i >= 1 && text[i - 1] == 'R') {
            const char before = i >= 2 ? text[i - 2] : '\0';
            if (i < 2 || !isIdent(before) || before == 'u' || before == 'U' || before == 'L' || before == '8') {
                const auto open = text.find('(', i);
                if (open != std::string::npos) {
                    const std::string closing = ")" + text.substr(i + 1, open - i - 1) + "\"";
                    const auto close = text.find(closing, open);
                    if (close != std::string::npos)
                        return close + closing.size();
                }
            }
        }
        std::size_t j = i + 1;
        while (j < text.size() && text[j] != '"' && text[j] != '\n')
            j += text[j] == '\\' ? 2 : 1;
        return std::min(j + 1, text.size());
    }
    if (c == '\'') {
        // A digit separator (1'000) is not a character literal.
        if (i >= 1 && isHex(text[i - 1]) && isHex(next))
            return i;
        std::size_t j = i + 1;
        while (j < text.size() && text[j] != '\'' && text[j] != '\n')
            j += text[j] == '\\' ? 2 : 1;
        return std::min(j + 1, text.size());
    }
    return i;
}

/** Index of the bracket that closes the '(' or '{' at open, or npos. Trivia is skipped. */
inline std::size_t matchClose(const std::string& text, std::size_t open)
{
    const char o = text[open];
    const char c = o == '(' ? ')' : '}';
    int depth = 0;
    for (std::size_t i = open; i < text.size();) {
        const auto j = skipTrivia(text, i);
        if (j != i) {
            i = j;
            continue;
        }
        if (text[i] == o) {
            ++depth;
        } else if (text[i] == c && --depth == 0) {
            return i;
        }
        ++i;
    }
    return std::string::npos;
}

inline std::string stripWhitespace(const std::string& s)
{
    std::string out;
    for (const char c : s) {
        if (!isAsciiSpace(c))
            out.push_back(c);
    }
    return out;
}

inline std::string trimAscii(const std::string& s)
{
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && isAsciiSpace(s[begin]))
        ++begin;
    while (end > begin && isAsciiSpace(s[end - 1]))
        --end;
    return s.substr(begin, end - begin);
}

/** Top-level comma separated parts of a parameter list (not inside <>, (), [] or {}). */
inline std::vector<std::string> splitParams(const std::string& list)
{
    std::vector<std::string> out;
    int depth = 0;
    std::string current;
    for (const char c : list) {
        if (c == '<' || c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == '>' || c == ')' || c == ']' || c == '}')
            --depth;
        if (c == ',' && depth == 0) {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!trimAscii(current).empty() || !out.empty())
        out.push_back(current);
    std::vector<std::string> trimmed;
    for (const auto& part : out) {
        auto t = trimAscii(part);
        if (!t.empty())
            trimmed.push_back(std::move(t));
    }
    return trimmed;
}

/** Whether the parameter list of the source has the parameter types that the symbol names; parameter names
    and default values are ignored. */
inline bool paramsMatch(const std::string& sourceList, const std::string& wantedList)
{
    const auto source = splitParams(sourceList);
    const auto wanted = splitParams(wantedList);
    if (source.size() != wanted.size())
        return false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        std::string noDefault = source[i];
        const auto equals = noDefault.find('=');
        if (equals != std::string::npos) {
            noDefault.erase(equals);
            while (!noDefault.empty() && isAsciiSpace(noDefault.back()))
                noDefault.pop_back();
        }
        std::string withoutName = noDefault;
        std::size_t nameBegin = withoutName.size();
        while (nameBegin > 0 && isIdent(withoutName[nameBegin - 1]))
            --nameBegin;
        if (nameBegin < withoutName.size() && nameBegin > 0
            && !(withoutName[nameBegin] >= '0' && withoutName[nameBegin] <= '9')) {
            const char before = withoutName[nameBegin - 1];
            if (isAsciiSpace(before) || before == '&' || before == '*' || before == '>')
                withoutName = withoutName.substr(0, nameBegin);
        }
        const auto want = stripWhitespace(wanted[i]);
        if (stripWhitespace(noDefault) != want && stripWhitespace(trimAscii(withoutName)) != want)
            return false;
    }
    return true;
}

/** Number of bytes of the Unicode white space code point that ends at position end of s (JavaScript's
    String.prototype.trim set), or 0. */
inline std::size_t trailingSpaceBytes(const std::string& s, std::size_t end)
{
    if (end == 0)
        return 0;
    if (isAsciiSpace(s[end - 1]))
        return 1;
    const auto byteAt = [&](std::size_t back) { return static_cast<unsigned char>(s[end - back]); };
    if (end >= 2 && byteAt(2) == 0xC2 && byteAt(1) == 0xA0) // U+00A0
        return 2;
    if (end >= 3) {
        const unsigned b1 = byteAt(3), b2 = byteAt(2), b3 = byteAt(1);
        if (b1 == 0xE1 && b2 == 0x9A && b3 == 0x80) // U+1680
            return 3;
        if (b1 == 0xE2 && b2 == 0x80 && ((b3 >= 0x80 && b3 <= 0x8A) || b3 == 0xA8 || b3 == 0xA9 || b3 == 0xAF))
            return 3;                               // U+2000..U+200A, U+2028, U+2029, U+202F
        if (b1 == 0xE2 && b2 == 0x81 && b3 == 0x9F) // U+205F
            return 3;
        if (b1 == 0xE3 && b2 == 0x80 && b3 == 0x80) // U+3000
            return 3;
        if (b1 == 0xEF && b2 == 0xBB && b3 == 0xBF) // U+FEFF
            return 3;
    }
    return 0;
}

inline std::size_t leadingSpaceBytes(const std::string& s, std::size_t begin)
{
    if (begin >= s.size())
        return 0;
    if (isAsciiSpace(s[begin]))
        return 1;
    const auto byteAt = [&](std::size_t n) { return static_cast<unsigned char>(s[begin + n]); };
    if (begin + 1 < s.size() && byteAt(0) == 0xC2 && byteAt(1) == 0xA0)
        return 2;
    if (begin + 2 < s.size()) {
        const unsigned b1 = byteAt(0), b2 = byteAt(1), b3 = byteAt(2);
        if ((b1 == 0xE1 && b2 == 0x9A && b3 == 0x80)
            || (b1 == 0xE2 && b2 == 0x80 && ((b3 >= 0x80 && b3 <= 0x8A) || b3 == 0xA8 || b3 == 0xA9 || b3 == 0xAF))
            || (b1 == 0xE2 && b2 == 0x81 && b3 == 0x9F) || (b1 == 0xE3 && b2 == 0x80 && b3 == 0x80)
            || (b1 == 0xEF && b2 == 0xBB && b3 == 0xBF))
            return 3;
    }
    return 0;
}

} // namespace detail

/** A located piece of source: its text (whole lines, or a declaration from the start of its first line) and
    its 1-based first and last line. */
struct Located
{
    std::string text;
    int startLine = 0;
    int endLine = 0;
};

/**
 * Finds the definition of the symbol in normalised text. symbol is the qualified name; the parameter types
 * follow in parentheses when overloads must be told apart. The definition is the first occurrence of
 * "name(" outside comments and literals whose parameter list is followed by a '{' before any ';'.
 */
inline std::optional<Located> extractFunction(const std::string& text, const std::string& symbol)
{
    using namespace detail;
    const auto paren = symbol.find('(');
    const std::string name = trimAscii(paren == std::string::npos ? symbol : symbol.substr(0, paren));
    std::optional<std::string> params;
    if (paren != std::string::npos)
        params = symbol.substr(paren + 1, symbol.rfind(')') - paren - 1);
    if (name.empty())
        return std::nullopt;
    std::size_t from = 0;
    while (true) {
        const auto m = text.find(name, from);
        if (m == std::string::npos)
            return std::nullopt;
        from = m + 1;
        std::size_t open = m + name.size();
        while (open < text.size() && isAsciiSpace(text[open]))
            ++open;
        if (open >= text.size() || text[open] != '(')
            continue;
        if (m > 0 && (isIdent(text[m - 1]) || text[m - 1] == ':' || text[m - 1] == '~'))
            continue;
        const std::size_t lineStart =
            m == 0 ? 0 : (text.rfind('\n', m - 1) == std::string::npos ? 0 : text.rfind('\n', m - 1) + 1);
        bool inTrivia = false;
        for (std::size_t i = lineStart; i < m;) {
            const auto j = skipTrivia(text, i);
            if (j > m) {
                inTrivia = true;
                break;
            }
            i = j != i ? j : i + 1;
        }
        if (inTrivia)
            continue;
        const auto close = matchClose(text, open);
        if (close == std::string::npos)
            continue;
        if (params && !paramsMatch(text.substr(open + 1, close - open - 1), *params))
            continue;
        // After the parameter list: qualifiers, a trailing return type, constructor initialisers, up to '{'.
        std::size_t bodyOpen = std::string::npos;
        int depth = 0;
        for (std::size_t i = close + 1; i < text.size();) {
            const auto j = skipTrivia(text, i);
            if (j != i) {
                i = j;
                continue;
            }
            const char c = text[i];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                // An unmatched ')' means that the name was a call argument, not a definition.
                if (--depth < 0)
                    break;
            } else if (depth == 0 && c == ';') {
                break;
            } else if (depth == 0 && c == '{') {
                bodyOpen = i;
                break;
            }
            ++i;
        }
        if (bodyOpen == std::string::npos)
            continue;
        const auto bodyClose = matchClose(text, bodyOpen);
        if (bodyClose == std::string::npos)
            continue;
        const auto lineEnd = text.find('\n', bodyClose);
        Located found;
        found.text = text.substr(lineStart, (lineEnd == std::string::npos ? text.size() : lineEnd) - lineStart);
        found.startLine = 1;
        for (std::size_t i = 0; i < m; ++i)
            found.startLine += text[i] == '\n' ? 1 : 0;
        found.endLine = 1;
        for (std::size_t i = 0; i < bodyClose; ++i)
            found.endLine += text[i] == '\n' ? 1 : 0;
        return found;
    }
}

/** Finds a declaration: the first line that holds the given text outside comments and literals, up to the
    first ';' outside brackets. */
inline std::optional<Located> extractStatement(const std::string& text, const std::string& anchor)
{
    using namespace detail;
    if (anchor.empty())
        return std::nullopt;
    for (std::size_t i = 0; i < text.size();) {
        const auto j = skipTrivia(text, i);
        if (j != i) {
            i = j;
            continue;
        }
        if (text.compare(i, anchor.size(), anchor) != 0) {
            ++i;
            continue;
        }
        // Word boundaries: "maxSize_Extra" or "xmaxSize_" must not match the anchor "maxSize_".
        const bool leftOpen = isIdent(anchor.front()) && i > 0 && isIdent(text[i - 1]);
        const bool rightOpen =
            isIdent(anchor.back()) && i + anchor.size() < text.size() && isIdent(text[i + anchor.size()]);
        if (leftOpen || rightOpen) {
            ++i;
            continue;
        }
        const std::size_t lineStart =
            i == 0 ? 0 : (text.rfind('\n', i - 1) == std::string::npos ? 0 : text.rfind('\n', i - 1) + 1);
        int depth = 0;
        for (std::size_t k = i; k < text.size();) {
            const auto s = skipTrivia(text, k);
            if (s != k) {
                k = s;
                continue;
            }
            const char c = text[k];
            if (c == '(' || c == '[' || c == '{')
                ++depth;
            else if (c == ')' || c == ']' || c == '}')
                --depth;
            else if (c == ';' && depth <= 0) {
                Located found;
                found.text = text.substr(lineStart, k + 1 - lineStart);
                found.startLine = 1;
                for (std::size_t n = 0; n < i; ++n)
                    found.startLine += text[n] == '\n' ? 1 : 0;
                found.endLine = 1;
                for (std::size_t n = 0; n < k; ++n)
                    found.endLine += text[n] == '\n' ? 1 : 0;
                return found;
            }
            ++k;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/** The normalised lines of a located text (see the top of this file), joined with LF. */
inline std::string normalizedLines(const std::string& located)
{
    using namespace detail;
    std::string out;
    std::size_t start = 0;
    while (start <= located.size()) {
        auto end = located.find('\n', start);
        if (end == std::string::npos)
            end = located.size();
        std::string line = located.substr(start, end - start);
        std::size_t begin = 0;
        while (const auto n = leadingSpaceBytes(line, begin))
            begin += n;
        std::size_t last = line.size();
        while (last > begin) {
            const auto n = trailingSpaceBytes(line, last);
            if (n == 0)
                break;
            last -= n;
        }
        std::string collapsed;
        bool pendingBlank = false;
        for (std::size_t i = begin; i < last; ++i) {
            if (line[i] == ' ' || line[i] == '\t') {
                pendingBlank = true;
            } else {
                if (pendingBlank)
                    collapsed.push_back(' ');
                pendingBlank = false;
                collapsed.push_back(line[i]);
            }
        }
        if (!collapsed.empty()) {
            if (!out.empty())
                out.push_back('\n');
            out += collapsed;
        }
        start = end + 1;
    }
    return out;
}

/** Hash of the located symbol in the file contents, or nullopt when the symbol is not found. kind is
    "function" (fn-v1) or "statement" (stmt-v1). */
inline std::optional<std::string> hashSymbol(const std::string& fileContents, const std::string& kind,
                                             const std::string& symbol)
{
    const auto text = normalizeSource(fileContents);
    const auto found = kind == "statement" ? extractStatement(text, symbol) : extractFunction(text, symbol);
    if (!found)
        return std::nullopt;
    return sha256Hex(normalizedLines(found->text));
}

} // namespace ControlMirror
} // namespace OpenTune
