// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlJson.h"

#include <cstdlib>
#include <locale.h>
#include <unordered_set>
#include <utility>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace OpenTune {

namespace {

bool isWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

// Converts a validated JSON number literal to binary64 with round-to-nearest, independent of the
// process locale (the decimal separator of the C locale is always '.').
double jsonNumberValue(std::string_view literal)
{
    const std::string copy(literal);
#if defined(_WIN32)
    static const _locale_t cLocale = _create_locale(LC_ALL, "C");
    return _strtod_l(copy.c_str(), nullptr, cLocale);
#else
    static const locale_t cLocale = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(0));
    return strtod_l(copy.c_str(), nullptr, cLocale);
#endif
}

void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class JsonParser
{
public:
    JsonParser(std::string_view text, int maxDepth) : text_(text), maxDepth_(maxDepth) {}

    bool parseDocument(JsonValue& out)
    {
        skipWhitespace();
        if (!parseValue(out, 1))
            return false;
        skipWhitespace();
        if (pos_ != text_.size())
            return fail("unexpected data after the JSON value");
        return true;
    }

    const char* error() const { return error_; }

private:
    bool fail(const char* why)
    {
        error_ = why;
        return false;
    }

    bool at(char c) const { return pos_ < text_.size() && text_[pos_] == c; }

    void skipWhitespace()
    {
        while (pos_ < text_.size() && isWhitespace(text_[pos_]))
            ++pos_;
    }

    bool parseValue(JsonValue& out, int depth)
    {
        if (pos_ >= text_.size())
            return fail("unexpected end of input");
        const char c = text_[pos_];
        switch (c) {
            case '{':
                return parseObject(out, depth);
            case '[':
                return parseArray(out, depth);
            case '"': {
                std::string s;
                if (!parseString(s))
                    return false;
                out = JsonValue::string(std::move(s));
                return true;
            }
            case 't':
                return parseLiteral("true", JsonValue::boolean(true), out);
            case 'f':
                return parseLiteral("false", JsonValue::boolean(false), out);
            case 'n':
                return parseLiteral("null", JsonValue(), out);
            default:
                if (c == '-' || isDigit(c))
                    return parseNumber(out);
                return fail("unexpected character");
        }
    }

    bool parseLiteral(std::string_view word, JsonValue value, JsonValue& out)
    {
        if (text_.substr(pos_, word.size()) != word)
            return fail("invalid literal");
        pos_ += word.size();
        out = std::move(value);
        return true;
    }

    bool parseObject(JsonValue& out, int depth)
    {
        if (depth > maxDepth_)
            return fail("nesting too deep");
        ++pos_;
        out = JsonValue::object();
        skipWhitespace();
        if (at('}')) {
            ++pos_;
            return true;
        }
        std::unordered_set<std::string> seen;
        for (;;) {
            skipWhitespace();
            if (!at('"'))
                return fail("expected a member name");
            std::string key;
            if (!parseString(key))
                return false;
            if (!seen.insert(key).second)
                return fail("duplicate member name");
            skipWhitespace();
            if (!at(':'))
                return fail("expected ':'");
            ++pos_;
            skipWhitespace();
            JsonValue value;
            if (!parseValue(value, depth + 1))
                return false;
            out.appendMember(std::move(key), std::move(value));
            skipWhitespace();
            if (at(',')) {
                ++pos_;
                continue;
            }
            if (at('}')) {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(JsonValue& out, int depth)
    {
        if (depth > maxDepth_)
            return fail("nesting too deep");
        ++pos_;
        out = JsonValue::array();
        skipWhitespace();
        if (at(']')) {
            ++pos_;
            return true;
        }
        for (;;) {
            skipWhitespace();
            JsonValue value;
            if (!parseValue(value, depth + 1))
                return false;
            out.append(std::move(value));
            skipWhitespace();
            if (at(',')) {
                ++pos_;
                continue;
            }
            if (at(']')) {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parseHex4(std::uint32_t& value)
    {
        if (pos_ + 4 > text_.size())
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        ++pos_; // opening quote
        for (;;) {
            if (pos_ >= text_.size())
                return fail("unterminated string");
            const auto c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20)
                return fail("control character in string");
            if (c != '\\') {
                const std::size_t start = pos_;
                while (pos_ < text_.size()) {
                    const auto d = static_cast<unsigned char>(text_[pos_]);
                    if (d == '"' || d == '\\' || d < 0x20)
                        break;
                    ++pos_;
                }
                out.append(text_.data() + start, pos_ - start);
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size())
                return fail("unterminated escape");
            const char e = text_[pos_++];
            switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parseHex4(cp))
                        return fail("invalid unicode escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t low = 0;
                        if (!at('\\') || pos_ + 1 >= text_.size() || text_[pos_ + 1] != 'u')
                            return fail("unpaired surrogate escape");
                        pos_ += 2;
                        if (!parseHex4(low) || low < 0xDC00 || low > 0xDFFF)
                            return fail("unpaired surrogate escape");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate escape");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape");
            }
        }
    }

    bool parseNumber(JsonValue& out)
    {
        const std::size_t start = pos_;
        if (at('-'))
            ++pos_;
        if (at('0')) {
            ++pos_;
        } else if (pos_ < text_.size() && text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && isDigit(text_[pos_]))
                ++pos_;
        } else {
            return fail("invalid number");
        }
        if (at('.')) {
            ++pos_;
            if (pos_ >= text_.size() || !isDigit(text_[pos_]))
                return fail("invalid number");
            while (pos_ < text_.size() && isDigit(text_[pos_]))
                ++pos_;
        }
        if (at('e') || at('E')) {
            ++pos_;
            if (at('+') || at('-'))
                ++pos_;
            if (pos_ >= text_.size() || !isDigit(text_[pos_]))
                return fail("invalid number");
            while (pos_ < text_.size() && isDigit(text_[pos_]))
                ++pos_;
        }
        const std::string_view literal = text_.substr(start, pos_ - start);
        out = JsonValue::number(std::string(literal), jsonNumberValue(literal));
        return true;
    }

    std::string_view text_;
    int maxDepth_;
    std::size_t pos_ = 0;
    const char* error_ = "";
};

void appendJson(std::string& out, const JsonValue& value)
{
    switch (value.type()) {
        case JsonValue::Type::Null:
            out += "null";
            break;
        case JsonValue::Type::Boolean:
            out += value.asBoolean() ? "true" : "false";
            break;
        case JsonValue::Type::Number:
            out += value.text();
            break;
        case JsonValue::Type::String:
            appendJsonQuoted(out, value.text());
            break;
        case JsonValue::Type::Array:
            out += '[';
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i > 0)
                    out += ',';
                appendJson(out, value.at(i));
            }
            out += ']';
            break;
        case JsonValue::Type::Object:
            out += '{';
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i > 0)
                    out += ',';
                appendJsonQuoted(out, value.keyAt(i));
                out += ':';
                appendJson(out, value.at(i));
            }
            out += '}';
            break;
    }
}

} // namespace

JsonValue JsonValue::boolean(bool value)
{
    JsonValue v;
    v.type_ = Type::Boolean;
    v.boolean_ = value;
    return v;
}

JsonValue JsonValue::integer(std::int64_t value)
{
    JsonValue v;
    v.type_ = Type::Number;
    v.number_ = static_cast<double>(value);
    v.text_ = std::to_string(value);
    return v;
}

JsonValue JsonValue::number(std::string literal, double value)
{
    JsonValue v;
    v.type_ = Type::Number;
    v.number_ = value;
    v.text_ = std::move(literal);
    return v;
}

JsonValue JsonValue::string(std::string utf8)
{
    JsonValue v;
    v.type_ = Type::String;
    v.text_ = std::move(utf8);
    return v;
}

JsonValue JsonValue::array()
{
    JsonValue v;
    v.type_ = Type::Array;
    return v;
}

JsonValue JsonValue::object()
{
    JsonValue v;
    v.type_ = Type::Object;
    return v;
}

const JsonValue* JsonValue::find(std::string_view key) const
{
    if (type_ != Type::Object)
        return nullptr;
    for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key)
            return &items_[i];
    return nullptr;
}

JsonValue& JsonValue::append(JsonValue value)
{
    items_.push_back(std::move(value));
    return *this;
}

JsonValue& JsonValue::set(std::string key, JsonValue value)
{
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(value);
            return *this;
        }
    }
    return appendMember(std::move(key), std::move(value));
}

JsonValue& JsonValue::appendMember(std::string key, JsonValue value)
{
    keys_.push_back(std::move(key));
    items_.push_back(std::move(value));
    return *this;
}

JsonParseResult parseJson(std::string_view text, int maxDepth)
{
    JsonParseResult result;
    if (!isValidUtf8(text)) {
        result.error = "not valid UTF-8";
        return result;
    }
    JsonParser parser(text, maxDepth);
    result.ok = parser.parseDocument(result.value);
    if (!result.ok) {
        result.error = parser.error();
        result.value = JsonValue();
    }
    return result;
}

bool isValidUtf8(std::string_view text)
{
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            length = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            length = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            length = 4;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + length > n)
            return false;
        for (std::size_t k = 1; k < length; ++k) {
            const auto cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0u) != 0x80u)
                return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (length == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)))
            return false;
        if (length == 4 && (cp < 0x10000 || cp > 0x10FFFF))
            return false;
        i += length;
    }
    return true;
}

std::size_t utf8CodePointCount(std::string_view text)
{
    std::size_t count = 0;
    for (const char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u)
            ++count;
    return count;
}

bool isJsonWhitespaceOnly(std::string_view text)
{
    for (const char c : text)
        if (!isWhitespace(c))
            return false;
    return true;
}

std::string toJsonString(const JsonValue& value)
{
    std::string out;
    appendJson(out, value);
    return out;
}

void appendJsonQuoted(std::string& out, std::string_view utf8)
{
    static const char* const hex = "0123456789abcdef";
    out += '"';
    for (const char ch : utf8) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0x0F];
                } else {
                    out += ch;
                }
                break;
        }
    }
    out += '"';
}

} // namespace OpenTune
