// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlJson.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace OpenTune {

/** Protocol version served by this implementation. */
inline constexpr int kControlProtocolMajor = 1;
inline constexpr const char* kControlProtocolVersion = "1.0";

/**
 * Limits of the control protocol, reported by session.hello.
 */
struct ControlLimits
{
    int preAuthReadBytes = 8192;
    int helloTimeoutMs = 2000;
    int maxPreAuthConnections = 8;
    int maxSessions = 4;
    int maxLineBytes = 4194304;
    int maxInFlightPerSession = 16;
    int idleTimeoutMs = 600000;
    /** Implementation-defined, 1000 to 30000 ms: how long a request may wait for the main thread. */
    int dispatchTimeoutMs = 10000;
    int jobWaitMaxMs = 30000;
    int jobRetentionMs = 1800000;
    int jobRetentionCount = 64;
};

/** The limits object of session.hello and session.info. */
JsonValue controlLimitsToJson(const ControlLimits& limits);

/** Capability of a method defined by protocol version 1, or nullptr when the name is not a defined method. */
const char* controlMethodCapability(std::string_view method);

/**
 * A request id: a string of 1 to 128 code points, or a number whose
 * binary64 value is an integer from 0 to 2^53-1. The default-constructed id is the null id of
 * responses that cannot carry the request's id.
 */
class ControlRequestId
{
public:
    ControlRequestId() = default;

    /** The id of an id member, or nullopt when the member is missing or not a valid id. */
    static std::optional<ControlRequestId> fromJson(const JsonValue* member);
    static ControlRequestId fromInteger(std::int64_t value);
    static ControlRequestId fromString(std::string value);

    bool isNull() const { return kind_ == Kind::Null; }
    bool isNumber() const { return kind_ == Kind::Number; }
    bool isString() const { return kind_ == Kind::String; }
    std::int64_t number() const { return number_; }
    const std::string& string() const { return text_; }

    /** Equal for two ids exactly when the ids are equal: the number 1.0 equals 1, and a number never
        equals a string. */
    std::string key() const;
    /** Appends the id as responses write it: a plain integer literal, a JSON string, or null. */
    void appendJson(std::string& out) const;

private:
    enum class Kind {
        Null,
        Number,
        String
    };
    Kind kind_ = Kind::Null;
    std::int64_t number_ = 0;
    std::string text_;
};

/** A valid request. params is always an object ({} when the
    request had none). */
struct ControlRequest
{
    ControlRequestId id;
    std::string method;
    JsonValue params = JsonValue::object();
};

/** True when the value is a notification: an object without an id member
    whose jsonrpc is "2.0" and whose method is a string. */
bool controlIsNotification(const JsonValue& value);

/**
 * Validates a request. On success fills request and returns true. On failure returns false,
 * sets errorId to the value's id when the value is an object with a valid id (otherwise null) and
 * sets why to a fixed ASCII explanation.
 */
bool controlParseRequest(const JsonValue& value, ControlRequest& request, ControlRequestId& errorId, std::string& why);

/** JSON-RPC error code of an error kind and reason. */
int controlErrorCode(std::string_view kind, std::string_view reason);
/** Default retryability of an error kind. */
bool controlErrorRetryableByDefault(std::string_view kind);
/** Retryability of an error kind and reason: the kind's default, except NOT_READY reasons that waiting never ends
    (f0_failed, f0_empty, grid_not_seeded and export_dry_stretched) and MODEL_UNAVAILABLE(vocoder_not_ready), which can
    succeed when asked again (MODEL_UNAVAILABLE(vocoder_load_failed) cannot: the same weight fails again). */
bool controlErrorRetryable(std::string_view kind, std::string_view reason);

/**
 * One error response line, without the LF. reason may be empty. detail is reduced to
 * printable ASCII and at most 1024 characters; callers must never put secrets or text taken from
 * files or projects into it. retryable defaults to the kind's default. extraData, when present, is an
 * object whose members are added to error.data as JSON without ASCII sanitization. Its members must
 * be operation-defined public data and must not contain secrets or text taken from files or projects.
 */
std::string controlErrorResponse(const ControlRequestId& id, std::string_view kind, std::string_view reason,
                                 std::string_view detail, bool executed, std::optional<bool> retryable = std::nullopt,
                                 const JsonValue* extraData = nullptr);

/** One success response line, without the LF. */
std::string controlResultResponse(const ControlRequestId& id, const JsonValue& result);

/** The fixed AUTH_FAILED and PROTOCOL_MISMATCH responses of Appendix A, without the LF. */
std::string controlAuthFailedResponse(const ControlRequestId& id);
std::string controlProtocolMismatchResponse(const ControlRequestId& id);

/** Outcome of validating session.hello credentials and request parameters. */
enum class ControlHelloVerdict {
    AuthFailed,
    ProtocolMismatch,
    InvalidParams,
    Accepted
};

/**
 * Validates the params of a session.hello request (nullptr when
 * the request has none). The token is compared in constant time whenever params.token is a string,
 * whatever the other members are. For InvalidParams, detail says what is wrong; it never quotes the
 * token.
 */
ControlHelloVerdict controlCheckHello(const JsonValue* params, std::string_view instanceToken, std::string& detail);

} // namespace OpenTune
