// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlProtocol.h"

#include "SecureRandom.h"

#include <cmath>

namespace OpenTune {

namespace {

struct MethodCapability
{
    const char* method;
    const char* capability;
};

// Every method of protocol version 1 (PROTOCOL.md section 22 and methods/index.json).
constexpr MethodCapability kMethods[] = {
    {"session.hello", "session.v1"},
    {"session.info", "session.v1"},
    {"project.get", "read.v1"},
    {"content.getNotes", "read.v1"},
    {"content.getPitchCurve", "read.v1"},
    {"content.segmentNotes", "read.v1"},
    {"content.getDetails", "read.v1"},
    {"transport.get", "read.v1"},
    {"prefs.get", "read.v1"},
    {"app.getLog", "read.v1"},
    {"job.get", "jobs.v1"},
    {"job.cancel", "jobs.v1"},
    {"import.start", "import.v1"},
    {"export.start", "export.v1"},
    {"ui.getState", "ui.bridge"},
    {"ui.dismiss", "ui.bridge"},
    {"content.setKey", "edit.v1"},
    {"content.autoTune", "edit.v1"},
    {"content.editNotes", "edit.v1"},
    {"content.setPitchShift", "edit.v1"},
    {"edit.getHistory", "history.v1"},
    {"edit.undo", "history.v1"},
    {"edit.redo", "history.v1"},
    {"content.shiftTiming", "timing.v1"},
    {"content.editTimeGrid", "timing.v1"},
    {"transport.control", "transport.v1"},
    {"transport.setTempo", "transport.v1"},
    {"transport.setLoop", "transport.v1"},
    {"arrangement.edit", "arrangement.v1"},
    {"arrangement.copy", "arrangement.v1"},
    {"track.set", "tracks.v1"},
    {"track.add", "tracks.v1"},
    {"track.duplicate", "tracks.v1"},
    {"track.delete", "tracks.v1"},
    {"ui.setSelection", "ui.bridge"},
    {"ui.setWorkspace", "ui.workspace"},
    {"project.save", "project.io"},
    {"project.saveAs", "project.io"},
    {"project.open", "project.io"},
    {"project.clearRecent", "project.io"},
    {"project.saveCopy", "project.copy"},
    {"prefs.set", "prefs.write"},
    {"audio.get", "audio.device"},
    {"audio.set", "audio.device"},
    {"app.quit", "app.quit"},
};

constexpr double kMaxIntegerId = 9007199254740991.0; // 2^53 - 1

bool matchesCharacterClass(std::string_view text, std::size_t minLength, std::size_t maxLength,
                           std::string_view extraCharacters)
{
    if (text.size() < minLength || text.size() > maxLength)
        return false;
    for (const char c : text) {
        const bool alphanumeric = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alphanumeric && extraCharacters.find(c) == std::string_view::npos)
            return false;
    }
    return true;
}

bool isLowercaseHex(std::string_view text)
{
    for (const char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

// "0" or a decimal without leading zeros.
bool isVersionNumber(std::string_view text)
{
    if (text.empty())
        return false;
    for (const char c : text)
        if (c < '0' || c > '9')
            return false;
    return text.size() == 1 || text[0] != '0';
}

// common.schema.json#/$defs/protocolVersion: MAJOR.MINOR.
bool parseProtocolVersion(std::string_view text, std::string& major)
{
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos)
        return false;
    const std::string_view majorText = text.substr(0, dot);
    const std::string_view minorText = text.substr(dot + 1);
    if (!isVersionNumber(majorText) || !isVersionNumber(minorText))
        return false;
    major = std::string(majorText);
    return true;
}

// methods/session.hello.schema.json#/$defs/params, given that token and protocol already passed.
bool validateHelloParams(const JsonValue& params, std::string& detail)
{
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& key = params.keyAt(i);
        if (key != "token" && key != "protocol" && key != "client") {
            detail = "params has a member other than token, protocol and client";
            return false;
        }
    }
    const auto& token = params.find("token")->text();
    if (token.size() < 32 || token.size() > 128 || token.size() % 2 != 0 || !isLowercaseHex(token)) {
        detail = "params.token is not 16 to 64 bytes of lowercase hexadecimal";
        return false;
    }
    const JsonValue* client = params.find("client");
    if (client == nullptr || !client->isObject()) {
        detail = "params.client must be an object with name and version";
        return false;
    }
    for (std::size_t i = 0; i < client->size(); ++i) {
        const auto& key = client->keyAt(i);
        if (key != "name" && key != "version") {
            detail = "params.client has a member other than name and version";
            return false;
        }
    }
    const JsonValue* name = client->find("name");
    if (name == nullptr || !name->isString() || !matchesCharacterClass(name->text(), 1, 64, "._@/-")) {
        detail = "params.client.name must be 1 to 64 characters of A-Z a-z 0-9 . _ @ / -";
        return false;
    }
    const JsonValue* version = client->find("version");
    if (version == nullptr || !version->isString() || !matchesCharacterClass(version->text(), 1, 64, ".+_-")) {
        detail = "params.client.version must be 1 to 64 characters of A-Z a-z 0-9 . + _ -";
        return false;
    }
    return true;
}

std::string sanitizeDetail(std::string_view detail)
{
    std::string out;
    out.reserve(detail.size() < 1024 ? detail.size() : 1024);
    for (const char c : detail) {
        if (out.size() == 1024)
            break;
        const auto u = static_cast<unsigned char>(c);
        out += (u >= 0x20 && u <= 0x7E) ? c : '?';
    }
    return out;
}

} // namespace

JsonValue controlLimitsToJson(const ControlLimits& limits)
{
    auto json = JsonValue::object();
    json.appendMember("preAuthReadBytes", JsonValue::integer(limits.preAuthReadBytes));
    json.appendMember("helloTimeoutMs", JsonValue::integer(limits.helloTimeoutMs));
    json.appendMember("maxPreAuthConnections", JsonValue::integer(limits.maxPreAuthConnections));
    json.appendMember("maxSessions", JsonValue::integer(limits.maxSessions));
    json.appendMember("maxLineBytes", JsonValue::integer(limits.maxLineBytes));
    json.appendMember("maxInFlightPerSession", JsonValue::integer(limits.maxInFlightPerSession));
    json.appendMember("idleTimeoutMs", JsonValue::integer(limits.idleTimeoutMs));
    json.appendMember("dispatchTimeoutMs", JsonValue::integer(limits.dispatchTimeoutMs));
    json.appendMember("jobWaitMaxMs", JsonValue::integer(limits.jobWaitMaxMs));
    json.appendMember("jobRetentionMs", JsonValue::integer(limits.jobRetentionMs));
    json.appendMember("jobRetentionCount", JsonValue::integer(limits.jobRetentionCount));
    return json;
}

const char* controlMethodCapability(std::string_view method)
{
    for (const auto& entry : kMethods)
        if (method == entry.method)
            return entry.capability;
    return nullptr;
}

std::optional<ControlRequestId> ControlRequestId::fromJson(const JsonValue* member)
{
    if (member == nullptr)
        return std::nullopt;
    if (member->isString()) {
        const std::size_t length = utf8CodePointCount(member->text());
        if (length < 1 || length > 128)
            return std::nullopt;
        return fromString(member->text());
    }
    if (member->isNumber()) {
        // The id is decided by its binary64 value, not by how it is written (1.0 and 1e2 are 1 and 100).
        const double value = member->asDouble();
        if (!(value >= 0.0 && value <= kMaxIntegerId) || std::floor(value) != value)
            return std::nullopt;
        return fromInteger(static_cast<std::int64_t>(value));
    }
    return std::nullopt;
}

ControlRequestId ControlRequestId::fromInteger(std::int64_t value)
{
    ControlRequestId id;
    id.kind_ = Kind::Number;
    id.number_ = value;
    return id;
}

ControlRequestId ControlRequestId::fromString(std::string value)
{
    ControlRequestId id;
    id.kind_ = Kind::String;
    id.text_ = std::move(value);
    return id;
}

std::string ControlRequestId::key() const
{
    switch (kind_) {
        case Kind::Number:
            return "n" + std::to_string(number_);
        case Kind::String:
            return "s" + text_;
        case Kind::Null:
            break;
    }
    return "null";
}

void ControlRequestId::appendJson(std::string& out) const
{
    switch (kind_) {
        case Kind::Number:
            out += std::to_string(number_);
            return;
        case Kind::String:
            appendJsonQuoted(out, text_);
            return;
        case Kind::Null:
            break;
    }
    out += "null";
}

bool controlIsNotification(const JsonValue& value)
{
    if (!value.isObject() || value.find("id") != nullptr)
        return false;
    const JsonValue* jsonrpc = value.find("jsonrpc");
    const JsonValue* method = value.find("method");
    return jsonrpc != nullptr && jsonrpc->isString() && jsonrpc->text() == "2.0" && method != nullptr
           && method->isString();
}

bool controlParseRequest(const JsonValue& value, ControlRequest& request, ControlRequestId& errorId, std::string& why)
{
    errorId = ControlRequestId();
    if (!value.isObject()) {
        why = "the request is not a JSON object";
        return false;
    }
    const JsonValue* idMember = value.find("id");
    const auto id = ControlRequestId::fromJson(idMember);
    if (id)
        errorId = *id;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto& key = value.keyAt(i);
        if (key != "jsonrpc" && key != "id" && key != "method" && key != "params") {
            why = "the request has a member other than jsonrpc, id, method and params";
            return false;
        }
    }
    const JsonValue* jsonrpc = value.find("jsonrpc");
    if (jsonrpc == nullptr || !jsonrpc->isString() || jsonrpc->text() != "2.0") {
        why = "jsonrpc must be the string \"2.0\"";
        return false;
    }
    if (!id) {
        why = "id must be a string of 1 to 128 characters or an integer from 0 to 2^53-1";
        return false;
    }
    const JsonValue* method = value.find("method");
    if (method == nullptr || !method->isString()) {
        why = "method must be a string";
        return false;
    }
    const JsonValue* params = value.find("params");
    if (params != nullptr && !params->isObject()) {
        why = "params must be an object";
        return false;
    }
    request.id = *id;
    request.method = method->text();
    request.params = params != nullptr ? *params : JsonValue::object();
    return true;
}

int controlErrorCode(std::string_view kind, std::string_view reason)
{
    if (kind == "INVALID_ARGUMENT") {
        if (reason == "parse_error")
            return -32700;
        if (reason == "invalid_request" || reason == "batch_unsupported" || reason == "duplicate_id"
            || reason == "line_too_long")
            return -32600;
        return -32602;
    }
    if (kind == "UNSUPPORTED" && reason == "unknown_method")
        return -32601;
    if (kind == "AUTH_FAILED")
        return -32001;
    if (kind == "PROTOCOL_MISMATCH")
        return -32002;
    if (kind == "INTERNAL")
        return -32603;
    return -32000;
}

bool controlErrorRetryableByDefault(std::string_view kind)
{
    return kind == "NOT_READY" || kind == "BUSY" || kind == "TIMEOUT";
}

bool controlErrorRetryable(std::string_view kind, std::string_view reason)
{
    if (kind == "NOT_READY"
        && (reason == "f0_failed" || reason == "f0_empty" || reason == "grid_not_seeded"
            || reason == "export_dry_stretched"))
        return false;
    // The vocoder was still being loaded when the wait ran out; asking again can succeed. Not so for
    // vocoder_load_failed (the load ended and failed: the same weight fails again), which keeps the default, false.
    if (kind == "MODEL_UNAVAILABLE" && reason == "vocoder_not_ready")
        return true;
    return controlErrorRetryableByDefault(kind);
}

std::string controlErrorResponse(const ControlRequestId& id, std::string_view kind, std::string_view reason,
                                 std::string_view detail, bool executed, std::optional<bool> retryable)
{
    std::string line = "{\"jsonrpc\":\"2.0\",\"id\":";
    id.appendJson(line);
    line += ",\"error\":{\"code\":";
    line += std::to_string(controlErrorCode(kind, reason));
    line += ",\"message\":";
    appendJsonQuoted(line, kind);
    line += ",\"data\":{\"kind\":";
    appendJsonQuoted(line, kind);
    if (!reason.empty()) {
        line += ",\"reason\":";
        appendJsonQuoted(line, reason);
    }
    line += ",\"retryable\":";
    line += retryable.value_or(controlErrorRetryable(kind, reason)) ? "true" : "false";
    line += ",\"detail\":";
    appendJsonQuoted(line, sanitizeDetail(detail));
    line += ",\"executed\":";
    line += executed ? "true" : "false";
    line += "}}}";
    return line;
}

std::string controlResultResponse(const ControlRequestId& id, const JsonValue& result)
{
    std::string line = "{\"jsonrpc\":\"2.0\",\"id\":";
    id.appendJson(line);
    line += ",\"result\":";
    line += toJsonString(result);
    line += '}';
    return line;
}

std::string controlAuthFailedResponse(const ControlRequestId& id)
{
    return controlErrorResponse(id, "AUTH_FAILED", {}, "authentication failed", false, false);
}

std::string controlProtocolMismatchResponse(const ControlRequestId& id)
{
    return controlErrorResponse(id, "PROTOCOL_MISMATCH", {}, "unsupported protocol major version", false, false);
}

ControlHelloVerdict controlCheckHello(const JsonValue* params, std::string_view instanceToken, std::string& detail)
{
    const bool paramsIsObject = params != nullptr && params->isObject();
    const JsonValue* token = paramsIsObject ? params->find("token") : nullptr;
    const JsonValue* protocol = paramsIsObject ? params->find("protocol") : nullptr;
    // Compare whenever the token is a string, before looking at anything else, so that the time
    // taken never depends on which check fails.
    const bool tokenIsString = token != nullptr && token->isString();
    const bool tokenMatches = tokenIsString && SecureRandom::constantTimeEquals(token->text(), instanceToken);
    std::string major;
    const bool protocolValid =
        protocol != nullptr && protocol->isString() && parseProtocolVersion(protocol->text(), major);
    if (!paramsIsObject || !tokenIsString || !protocolValid || !tokenMatches)
        return ControlHelloVerdict::AuthFailed;
    if (major != std::to_string(kControlProtocolMajor))
        return ControlHelloVerdict::ProtocolMismatch;
    if (!validateHelloParams(*params, detail))
        return ControlHelloVerdict::InvalidParams;
    return ControlHelloVerdict::Accepted;
}

} // namespace OpenTune
