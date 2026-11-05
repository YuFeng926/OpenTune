// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OpenTune {

/**
 * A JSON value (RFC 8259) as exchanged on the control connection.
 *
 * Objects keep their members in the order in which they were parsed or added. Numbers keep both
 * their literal text and their value as an IEEE 754 binary64 number: the protocol decides some
 * questions by the value (whether a request id is an integer) and writes integers back as plain
 * literals.
 *
 * Threading: a plain value type without internal synchronisation; share between threads only as
 * const.
 */
class JsonValue
{
public:
    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    /** Creates null. */
    JsonValue() = default;

    static JsonValue boolean(bool value);
    /** An integer, written as a plain decimal literal. */
    static JsonValue integer(std::int64_t value);
    /** A number with its literal text (a valid JSON number) and its binary64 value. */
    static JsonValue number(std::string literal, double value);
    /** A string. The text must be valid UTF-8. */
    static JsonValue string(std::string utf8);
    static JsonValue array();
    static JsonValue object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBoolean() const { return type_ == Type::Boolean; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBoolean() const { return boolean_; }
    /** The binary64 value of a number (0 for other types). */
    double asDouble() const { return number_; }
    /** The content of a string, or the literal text of a number. */
    const std::string& text() const { return text_; }

    /** Number of array elements or object members. */
    std::size_t size() const { return items_.size(); }
    /** Array element, or value of the object member at that position. */
    const JsonValue& at(std::size_t index) const { return items_[index]; }
    /** Name of the object member at that position. */
    const std::string& keyAt(std::size_t index) const { return keys_[index]; }
    /** Object member by name, or nullptr when absent or when this is not an object. */
    const JsonValue* find(std::string_view key) const;

    /** Appends an array element. Returns *this. */
    JsonValue& append(JsonValue value);
    /** Sets an object member, replacing a member of the same name. Returns *this. */
    JsonValue& set(std::string key, JsonValue value);
    /** Appends an object member without looking for an existing one; the caller guarantees that
        the name is new. Returns *this. */
    JsonValue& appendMember(std::string key, JsonValue value);

private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    double number_ = 0.0;
    std::string text_;
    std::vector<std::string> keys_;
    std::vector<JsonValue> items_;
};

/** Deepest nesting of arrays and objects that parseJson accepts; the outermost value has depth 1. */
inline constexpr int kJsonMaxDepth = 64;

/** Result of parseJson. error is a fixed ASCII description that never quotes the input. */
struct JsonParseResult
{
    bool ok = false;
    JsonValue value;
    const char* error = "";
};

/**
 * Parses exactly one JSON value, optionally surrounded by JSON whitespace (space, tab, CR, LF).
 *
 * Strict: the text must be valid UTF-8 (no byte order mark), objects must not repeat a member
 * name, string escapes must not leave an unpaired surrogate, and arrays and objects must not nest
 * deeper than maxDepth. Recursion is bounded by maxDepth. Thread-safe (no shared state).
 */
JsonParseResult parseJson(std::string_view text, int maxDepth = kJsonMaxDepth);

/** True when the bytes are well-formed UTF-8 (no overlong forms, surrogates or values above
    U+10FFFF). */
bool isValidUtf8(std::string_view text);

/** Number of Unicode code points in valid UTF-8 text. */
std::size_t utf8CodePointCount(std::string_view text);

/** True when the text is empty or contains only JSON whitespace. */
bool isJsonWhitespaceOnly(std::string_view text);

/** Serialises a value compactly (no whitespace between tokens). Non-ASCII text is written as
    UTF-8; '"', '\\' and control characters are escaped. */
std::string toJsonString(const JsonValue& value);

/** Appends a JSON string literal for UTF-8 text. */
void appendJsonQuoted(std::string& out, std::string_view utf8);

} // namespace OpenTune
