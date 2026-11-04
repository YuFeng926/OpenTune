// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace OpenTune {

/** The kinds of handle that name content-domain objects (PROTOCOL.md section 9). */
enum class ControlHandleKind {
    /** c:<instanceIdShort>:<contentEpoch>:<objectId>, a content of the StandaloneContentRepository. */
    Content,
    /** p:<instanceIdShort>:<contentEpoch>:<placementSerial>, a placement of the StandaloneArrangement. */
    Placement
};

/**
 * A contentId or placementId as a request gives it, after the form check of the schema pattern
 * (common.schema.json#/$defs/contentId and #/$defs/placementId). Resolution against the instance
 * (instance part, epoch, existence) happens later, on the message thread.
 *
 * The pattern allows decimal numbers of any length. A number that does not fit 64 bits is of valid
 * form but can never name anything; overflow records that, so that resolution reports it as stale
 * (epoch) or unknown (object) instead of rejecting the form.
 */
struct ControlHandle
{
    ControlHandleKind kind = ControlHandleKind::Content;
    /** The 8 lowercase hexadecimal digits after the prefix. */
    std::string instanceShort;
    std::uint64_t epoch = 0;
    std::uint64_t object = 0;
    bool epochOverflow = false;
    bool objectOverflow = false;
};

/** Parses a handle of the given kind; nullopt when the text does not have that kind's form. Pure. */
std::optional<ControlHandle> controlParseHandle(ControlHandleKind kind, std::string_view text);

/** The instanceIdShort of an instance id: its first 8 characters (PROTOCOL.md section 9). */
std::string controlInstanceShort(std::string_view instanceId);

/** Formats a handle of the given kind. Pure. */
std::string controlFormatHandle(ControlHandleKind kind, std::string_view instanceShort, std::uint64_t epoch,
                                std::uint64_t object);

/**
 * Builds an opaque revision token (common.schema.json#/$defs/revisionToken): a one-letter prefix
 * followed by 16 lowercase hexadecimal digits of a 64-bit FNV-1a hash over the fields added, in order.
 * Equal fields give equal tokens; clients compare tokens only for equality (PROTOCOL.md section 10).
 * Floating-point fields are hashed by value (0.0 and -0.0 are equal; NaN is hashed as one value).
 * Pure; not thread-safe (a value type).
 */
class ControlTokenHasher
{
public:
    explicit ControlTokenHasher(char prefix);

    ControlTokenHasher& add(std::uint64_t value);
    ControlTokenHasher& add(std::int64_t value);
    ControlTokenHasher& add(int value) { return add(static_cast<std::int64_t>(value)); }
    ControlTokenHasher& add(bool value) { return add(static_cast<std::uint64_t>(value ? 1 : 0)); }
    ControlTokenHasher& add(double value);
    /** Adds the length, then the bytes, so that the boundaries between strings count. */
    ControlTokenHasher& add(std::string_view text);

    /** The token: prefix and 16 hexadecimal digits. */
    std::string token() const;

private:
    void addBytes(const void* data, std::size_t size);

    char prefix_;
    std::uint64_t hash_;
};

/**
 * A pagination cursor (PROTOCOL.md section 10 rule 6): `<kind>1.<offset>.<paramsHash>.<tokensHash>`,
 * where kind is one lowercase letter that names the method, offset is the position of the next item,
 * paramsHash binds the cursor to the selection params of the request that issued it and tokensHash
 * to the tokens of that read. Opaque to clients.
 */
struct ControlCursor
{
    char kind = 'x';
    std::uint64_t offset = 0;
    std::string paramsHash;
    std::string tokensHash;
};

/** Formats a cursor; the hashes are tokens of ControlTokenHasher. Pure. */
std::string controlFormatCursor(const ControlCursor& cursor);

/** Parses a cursor of the given kind; nullopt when the text is not one. Pure. */
std::optional<ControlCursor> controlParseCursor(char kind, std::string_view text);

} // namespace OpenTune
