// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Unit tests of the control API channel: run-time options, path rules, CSPRNG, JSON, request ids,
// fixed responses, discovery and ready files, the listener with its rules before and after
// authentication, the content handles, revision tokens and cursors, and the redacting log tail. The session vectors of
// the specification copy (Tests/Control/spec/otcp/v1/vectors/ session) are read and run against a real server with a
// stand-in request handler. The rule by which the editor's bridge takes the changed mark of a view switch back is a
// plain function of values, and is tested here as well.
//
// Every file goes to a fresh directory under OPENTUNE_DATA_DIR when that variable is set, or under
// the build directory, never to the user's real data directories.

#include "ControlTestSupport.h"

#include "Control/ContentHandles.h"
#include "Control/ControlLogTail.h"
#include "Control/ControlPathPolicy.h"
#include "Standalone/ControlEditorBridgeViewSwitch.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <new>
#include <set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace OpenTune;
using namespace OpenTune::ControlTest;

// --------------------------------------------------------------------------------------------------
// Allocation failure injection
// --------------------------------------------------------------------------------------------------
// The connection exception test makes a connection thread fail the way a huge request line can fail
// on a loaded machine: with std::bad_alloc from its growing read buffer. While armed, the first
// allocation of at least the armed size fails and disarms; otherwise this is plain malloc and free.

namespace {
std::atomic<std::size_t> failAllocationAtLeast{0};
} // namespace

void* operator new(std::size_t size)
{
    std::size_t threshold = failAllocationAtLeast.load();
    if (threshold != 0 && size >= threshold && failAllocationAtLeast.compare_exchange_strong(threshold, 0))
        throw std::bad_alloc();
    for (;;) {
        if (void* memory = std::malloc(size != 0 ? size : 1))
            return memory;
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr)
            throw std::bad_alloc();
        handler();
    }
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace {

// Timing bounds of the server tests.
constexpr int kSlowDripIntervalMs = 300;
constexpr int kHelloTimeoutLowerBoundMs = 1800;
constexpr int kHelloTimeoutUpperBoundMs = 3500;
constexpr int kPromptCloseMs = 1500;
constexpr int kStopBoundMs = 1500;
constexpr int kTestIdleTimeoutMs = 600;
constexpr int kFloodThreads = 4;
constexpr int kFloodConnectionsPerThread = 50;
constexpr int kFloodResponseBoundMs = 2000;
constexpr int kCounterSettleMs = 3000;
constexpr int kCompletionBoundMs = 200;
/** Longer than two poll intervals of the server: a session that closed on the stop flag alone would
    be gone after this wait inside serverStopping(). */
constexpr int kStopWindowWaitMs = 600;
/** Time for bytes sent over loopback to reach the server, and for a stop step to follow. */
constexpr int kLoopbackSettleMs = 100;
/** A request line shorter than maxLineBytes whose read buffer meets the injected allocation failure. */
constexpr std::size_t kInjectedLineBytes = 3 * 1024 * 1024;
constexpr std::size_t kInjectedFailureBytes = 2 * 1024 * 1024;
/** Time for a connection thread to end (and its stack to be released) after its counters dropped. */
constexpr int kThreadExitSettleMs = 200;

// Appendix A of PROTOCOL.md, with id 1.
const char* const kAuthFailedId1 =
    R"({"jsonrpc":"2.0","id":1,"error":{"code":-32001,"message":"AUTH_FAILED","data":{"kind":"AUTH_FAILED","retryable":false,"detail":"authentication failed","executed":false}}})";
const char* const kProtocolMismatchId1 =
    R"({"jsonrpc":"2.0","id":1,"error":{"code":-32002,"message":"PROTOCOL_MISMATCH","data":{"kind":"PROTOCOL_MISMATCH","retryable":false,"detail":"unsupported protocol major version","executed":false}}})";

ControlEnvironmentLookup environmentOf(std::map<std::string, std::string> values)
{
    return [values](const char* name) -> std::optional<std::string> {
        const auto it = values.find(name);
        if (it == values.end())
            return std::nullopt;
        return it->second;
    };
}

JsonValue parsed(const std::string& text)
{
    const auto result = parseJson(text);
    CONTROL_CHECK(result.ok);
    return result.value;
}

std::string requestLine(const std::string& idJson, const std::string& method, const std::string& paramsJson = "{}")
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"method\":\"" + method + "\",\"params\":" + paramsJson + "}";
}

// --------------------------------------------------------------------------------------------------
// Options and paths
// --------------------------------------------------------------------------------------------------

void testOptions()
{
    const auto windows = ControlPathStyle::Windows;
    const auto posix = ControlPathStyle::Posix;
    {
        const auto options = parseControlApiOptions({}, environmentOf({}), windows);
        CONTROL_CHECK(!options.enabled && options.error.empty() && !options.canStart());
    }
    CONTROL_CHECK(parseControlApiOptions({}, environmentOf({{"OPENTUNE_CONTROL_API", "1"}}), windows).canStart());
    CONTROL_CHECK(!parseControlApiOptions({}, environmentOf({{"OPENTUNE_CONTROL_API", "true"}}), windows).enabled);
    CONTROL_CHECK(!parseControlApiOptions({}, environmentOf({{"OPENTUNE_CONTROL_API", "1 "}}), windows).enabled);
    CONTROL_CHECK(!parseControlApiOptions({"--control-api=1"}, environmentOf({}), windows).enabled);
    CONTROL_CHECK(parseControlApiOptions({"--control-api"}, environmentOf({}), windows).canStart());

    // While the API is off, its options are ignored (no error), but OPENTUNE_DATA_DIR is still read.
    {
        const auto options = parseControlApiOptions({"--control-api-dir=relative", "--control-api-launch-nonce=x"},
                                                    environmentOf({{"OPENTUNE_DATA_DIR", "relative"}}), windows);
        CONTROL_CHECK(!options.enabled && options.error.empty() && !options.discoveryDirectory && !options.launchNonce);
        CONTROL_CHECK(options.dataDirectory == std::optional<std::string>("relative"));
    }
    {
        // Every allowed character class, built at run time (vectors and tests never hold secret-like literals).
        const std::string nonce = std::string(8, 'a') + std::string(8, 'Z') + "09_-";
        const auto options = parseControlApiOptions({"--control-api", "--control-api-dir=C:\\Data\\instances",
                                                     "--control-api-ready-file=C:\\Data\\ready.json",
                                                     "--control-api-launch-nonce=" + nonce},
                                                    environmentOf({{"OPENTUNE_DATA_DIR", "C:\\Data"}}), windows);
        CONTROL_CHECK(options.canStart());
        CONTROL_CHECK(options.discoveryDirectory == std::optional<std::string>("C:\\Data\\instances"));
        CONTROL_CHECK(options.readyFile == std::optional<std::string>("C:\\Data\\ready.json"));
        CONTROL_CHECK(options.launchNonce == std::optional<std::string>(nonce));
    }
    CONTROL_CHECK(
        parseControlApiOptions({"--control-api", "--control-api-dir=/tmp/x"}, environmentOf({}), posix).canStart());

    const auto failsWith = [](std::vector<std::string> args, std::map<std::string, std::string> env,
                              ControlPathStyle style, const std::string& fragment) {
        args.insert(args.begin(), "--control-api");
        const auto options = parseControlApiOptions(args, environmentOf(std::move(env)), style);
        return options.enabled && !options.canStart() && options.error.find(fragment) != std::string::npos;
    };
    CONTROL_CHECK(failsWith({"--control-api-dir=relative\\dir"}, {}, windows, "path_not_absolute"));
    CONTROL_CHECK(failsWith({"--control-api-dir=/tmp/x"}, {}, windows, "path_not_absolute"));
    CONTROL_CHECK(failsWith({"--control-api-dir=C:\\a\\..\\b"}, {}, windows, "path_not_normalized"));
    CONTROL_CHECK(failsWith({"--control-api-dir="}, {}, windows, "path_not_absolute"));
    CONTROL_CHECK(failsWith({"--control-api-ready-file=\\\\server\\share\\r.json"}, {}, windows, "path_unc"));
    CONTROL_CHECK(failsWith({"--control-api-dir=relative"}, {}, posix, "path_not_absolute"));
    CONTROL_CHECK(failsWith({"--control-api-launch-nonce=short"}, {}, windows, "launch-nonce"));
    CONTROL_CHECK(failsWith({"--control-api-launch-nonce=abcdefghijklmnop!"}, {}, windows, "launch-nonce"));
    CONTROL_CHECK(failsWith({"--control-api-launch-nonce=" + std::string(129, 'a')}, {}, windows, "launch-nonce"));
    CONTROL_CHECK(
        parseControlApiOptions({"--control-api", "--control-api-launch-nonce=" + std::string(128, 'a')}, {}, windows)
            .canStart());
    CONTROL_CHECK(failsWith({"--control-api-dir=C:\\a", "--control-api-dir=C:\\b"}, {}, windows, "more than once"));
    CONTROL_CHECK(failsWith({"--control-api-dir"}, {}, windows, "unknown control API option"));
    CONTROL_CHECK(failsWith({"--control-api-dirx=C:\\a"}, {}, windows, "unknown control API option"));
    CONTROL_CHECK(failsWith({}, {{"OPENTUNE_DATA_DIR", "data"}}, windows, "OPENTUNE_DATA_DIR"));
    CONTROL_CHECK(failsWith({}, {{"OPENTUNE_DATA_DIR", ""}}, windows, "OPENTUNE_DATA_DIR"));
    // The error names the option and the rule, never the value.
    {
        const auto options =
            parseControlApiOptions({"--control-api", "--control-api-dir=C:\\private-value\\x."}, {}, windows);
        CONTROL_CHECK(!options.canStart() && options.error.find("private-value") == std::string::npos);
    }
}

void testPathPolicy()
{
    struct Case
    {
        const char* path;
        ControlPathStyle style;
        const char* reason;
    };
    const auto w = ControlPathStyle::Windows;
    const auto p = ControlPathStyle::Posix;
    const Case cases[] = {
        {"C:\\a\\b", w, ""},
        {"z:\\Music\\take 1.wav", w, ""},
        {"C:\\a\\b\x01", w, "path_invalid_character"},
        {"C:\\a\\\xff", w, "path_invalid_character"},
        {"\\\\?\\C:\\a", w, "path_device_namespace"},
        {"\\\\.\\pipe\\x", w, "path_device_namespace"},
        {"\\\\server\\share\\a", w, "path_unc"},
        {"C:a", w, "path_drive_relative"},
        {"C:", w, "path_drive_relative"},
        {"C:/a", w, "path_drive_relative"},
        {"a\\b", w, "path_not_absolute"},
        {"/a/b", w, "path_not_absolute"},
        {"", w, "path_not_absolute"},
        {"C:\\a\\b:stream", w, "path_alternate_stream"},
        {"C:\\a/b", w, "path_not_normalized"},
        {"C:\\a\\\\b", w, "path_not_normalized"},
        {"C:\\a\\.\\b", w, "path_not_normalized"},
        {"C:\\a\\..\\b", w, "path_not_normalized"},
        {"C:\\a\\", w, "path_not_normalized"},
        {"C:\\", w, "path_not_normalized"},
        {"C:\\a.", w, "path_not_normalized"},
        {"C:\\a ", w, "path_not_normalized"},
        {"C:\\a\\NUL.wav", w, "path_reserved_name"},
        {"C:\\a\\con", w, "path_reserved_name"},
        {"C:\\a\\COM1", w, "path_reserved_name"},
        {"C:\\a\\conout$", w, "path_reserved_name"},
        {"C:\\a\\nul .txt", w, "path_reserved_name"},
        {"C:\\a\\LPT\xC2\xB9", w, "path_reserved_name"},
        {"C:\\a\\COM10", w, ""},
        {"C:\\a\\console", w, ""},
        {"/a/b", p, ""},
        {"/a/b c/.hidden", p, ""},
        {"a/b", p, "path_not_absolute"},
        {"", p, "path_not_absolute"},
        {"/a//b", p, "path_not_normalized"},
        {"/a/./b", p, "path_not_normalized"},
        {"/a/../b", p, "path_not_normalized"},
        {"/a/", p, "path_not_normalized"},
        {"/", p, "path_not_normalized"},
        {"/a\x1f", p, "path_invalid_character"},
    };
    for (const auto& c : cases) {
        const auto reason = controlCheckAbsolutePath(c.path, c.style);
        if (!CONTROL_CHECK(reason == c.reason))
            note(std::string("path case expected '") + c.reason + "', got '" + reason + "'");
    }
    CONTROL_CHECK(controlJoinPath("C:\\a", "control/instances", w) == "C:\\a\\control\\instances");
    CONTROL_CHECK(controlJoinPath("/a/", "control/instances", p) == "/a/control/instances");
}

// --------------------------------------------------------------------------------------------------
// CSPRNG, JSON, ids and responses
// --------------------------------------------------------------------------------------------------

void testSecureRandom()
{
    const auto isLowerHex = [](const std::string& text) {
        return std::all_of(text.begin(), text.end(),
                           [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    const auto token = SecureRandom::hex(32);
    const auto proof = SecureRandom::hex(16);
    CONTROL_CHECK(token && token->size() == 64 && isLowerHex(*token));
    CONTROL_CHECK(proof && proof->size() == 32 && isLowerHex(*proof));
    std::set<std::string> seen;
    for (int i = 0; i < 2000; ++i)
        seen.insert(SecureRandom::hex(16).value_or(""));
    CONTROL_CHECK(seen.size() == 2000);
    // Every byte value appears in 64 KiB of output (a broken generator usually fails this).
    std::vector<unsigned char> bytes(65536);
    CONTROL_CHECK(SecureRandom::fill(bytes.data(), bytes.size()));
    std::set<unsigned char> values(bytes.begin(), bytes.end());
    CONTROL_CHECK(values.size() == 256);

    CONTROL_CHECK(SecureRandom::constantTimeEquals("abcdef", "abcdef"));
    CONTROL_CHECK(SecureRandom::constantTimeEquals("", ""));
    CONTROL_CHECK(!SecureRandom::constantTimeEquals("abcdee", "abcdef"));
    CONTROL_CHECK(!SecureRandom::constantTimeEquals("bbcdef", "abcdef"));
    CONTROL_CHECK(!SecureRandom::constantTimeEquals("abcde", "abcdef"));
    CONTROL_CHECK(!SecureRandom::constantTimeEquals("abcdefa", "abcdef"));
    CONTROL_CHECK(!SecureRandom::constantTimeEquals("", "abcdef"));
}

void testJson()
{
    const std::string compact = R"({"a":1,"b":[true,false,null],"c":"x","d":{"e":-1.5e3}})";
    const auto value = parsed(" \t" + compact + " \r");
    CONTROL_CHECK(toJsonString(value) == compact);
    CONTROL_CHECK(value.find("d")->find("e")->asDouble() == -1500.0);
    CONTROL_CHECK(parsed("1.0").asDouble() == 1.0 && parsed("1.0").text() == "1.0");
    CONTROL_CHECK(parsed("1e2").asDouble() == 100.0);

    const auto nested = [](int depth, char open, char close) {
        return std::string(static_cast<std::size_t>(depth), open) + std::string(static_cast<std::size_t>(depth), close);
    };
    CONTROL_CHECK(parseJson(nested(64, '[', ']')).ok);
    CONTROL_CHECK(!parseJson(nested(65, '[', ']')).ok);
    // Deep input fails fast instead of exhausting the stack.
    CONTROL_CHECK(!parseJson(nested(100000, '[', ']')).ok);

    const char* const invalid[] = {
        "",
        "{",
        "[1,]",
        "{\"a\":1,}",
        "01",
        "1.",
        ".5",
        "-",
        "1e",
        "+1",
        "tru",
        "nul",
        "'a'",
        "{a:1}",
        "[1 2]",
        "\"\x01\"",
        "\"\\x\"",
        "\"\\u12\"",
        "1 2",
        "\xEF\xBB\xBF{}",
        "\"\xff\"",
        "\"\xC0\xAF\"",
        "\"\xED\xA0\x80\"",
        "\"\\ud800\"",
        "\"\\udc00\"",
        "\"\\ud800\\u0041\"",
        "{\"a\":1,\"a\":2}",
    };
    for (const char* text : invalid) {
        if (!CONTROL_CHECK(!parseJson(text).ok))
            note(std::string("accepted invalid JSON: ") + text);
    }
    CONTROL_CHECK(parsed("\"\\ud834\\udd1e\"").text() == "\xF0\x9D\x84\x9E");
    CONTROL_CHECK(parsed("\"\\u00e9\\n\"").text() == "\xC3\xA9\n");

    std::string quoted;
    appendJsonQuoted(quoted, std::string("a\"b\\c\nd\x01\x7f\xC3\xA9", 11));
    CONTROL_CHECK(quoted == "\"a\\\"b\\\\c\\nd\\u0001\x7f\xC3\xA9\"");
    CONTROL_CHECK(isJsonWhitespaceOnly(" \t\r") && !isJsonWhitespaceOnly(" x"));
    CONTROL_CHECK(utf8CodePointCount("a\xC3\xA9\xF0\x9D\x84\x9E") == 3);
}

void testRequestIds()
{
    const auto idOf = [](const std::string& json) {
        const auto value = parseJson(json);
        return ControlRequestId::fromJson(&value.value);
    };
    const auto written = [](const ControlRequestId& id) {
        std::string out;
        id.appendJson(out);
        return out;
    };
    CONTROL_CHECK(idOf("1.0") && written(*idOf("1.0")) == "1");
    CONTROL_CHECK(idOf("1e2") && written(*idOf("1e2")) == "100");
    CONTROL_CHECK(idOf("0") && written(*idOf("0")) == "0");
    CONTROL_CHECK(idOf("9007199254740991") && written(*idOf("9007199254740991")) == "9007199254740991");
    CONTROL_CHECK(!idOf("9007199254740992"));
    CONTROL_CHECK(!idOf("1.5"));
    CONTROL_CHECK(!idOf("-1"));
    CONTROL_CHECK(!idOf("true"));
    CONTROL_CHECK(!idOf("null"));
    CONTROL_CHECK(!idOf("\"\""));
    CONTROL_CHECK(!ControlRequestId::fromJson(nullptr));
    std::string clefs;
    for (int i = 0; i < 128; ++i)
        clefs += "\\ud834\\udd1e";
    CONTROL_CHECK(idOf("\"" + clefs + "\"").has_value());
    CONTROL_CHECK(idOf("\"" + std::string(128, 'x') + "\"").has_value());
    CONTROL_CHECK(!idOf("\"" + std::string(129, 'x') + "\""));
    CONTROL_CHECK(idOf("1.0")->key() == idOf("1")->key());
    CONTROL_CHECK(idOf("\"1\"")->key() != idOf("1")->key());
    CONTROL_CHECK(written(*idOf("\"a\\\"b\"")) == "\"a\\\"b\"");
}

void testProtocol()
{
    const auto one = ControlRequestId::fromInteger(1);
    CONTROL_CHECK(controlAuthFailedResponse(one) == kAuthFailedId1);
    CONTROL_CHECK(controlProtocolMismatchResponse(one) == kProtocolMismatchId1);
    CONTROL_CHECK(controlAuthFailedResponse(ControlRequestId::fromString("h1")).find("\"id\":\"h1\"")
                  != std::string::npos);

    CONTROL_CHECK(controlErrorCode("INVALID_ARGUMENT", "parse_error") == -32700);
    CONTROL_CHECK(controlErrorCode("INVALID_ARGUMENT", "duplicate_id") == -32600);
    CONTROL_CHECK(controlErrorCode("INVALID_ARGUMENT", "invalid_params") == -32602);
    CONTROL_CHECK(controlErrorCode("UNSUPPORTED", "unknown_method") == -32601);
    CONTROL_CHECK(controlErrorCode("UNSUPPORTED", "capability_missing") == -32000);
    // no_editor_window: UNSUPPORTED is not retryable by default, and the registry says the same of this reason.
    CONTROL_CHECK(controlErrorCode("UNSUPPORTED", "no_editor_window") == -32000);
    CONTROL_CHECK(!controlErrorRetryable("UNSUPPORTED", "no_editor_window"));
    CONTROL_CHECK(controlErrorCode("INTERNAL", "") == -32603);
    // Retryability of the time-grid reasons: a stretched grid is not cured by a retry, a dropped rebuild and a
    // timed-out timing analysis are, and a failed timing analysis is not.
    CONTROL_CHECK(!controlErrorRetryable("NOT_READY", "export_dry_stretched"));
    CONTROL_CHECK(controlErrorRetryable("NOT_READY", "export_stretch_dropped"));
    CONTROL_CHECK(controlErrorRetryable("NOT_READY", "timing_analysis_timeout"));
    CONTROL_CHECK(!controlErrorRetryable("ANALYSIS_FAILED", "timing_analysis_failed"));
    CONTROL_CHECK(!controlErrorRetryable("INVALID_ARGUMENT", "nothing_to_export"));
    CONTROL_CHECK(!controlErrorRetryable("INVALID_ARGUMENT", "content_key_unset"));
    // The preference options: a vocoder that is not back in time may be ready when asked again; a weight that could
    // not be loaded fails again, a pitch model that cannot be switched is not cured by a retry, and neither is a value
    // that is not on offer.
    CONTROL_CHECK(controlErrorRetryable("MODEL_UNAVAILABLE", "vocoder_not_ready"));
    CONTROL_CHECK(!controlErrorRetryable("MODEL_UNAVAILABLE", "vocoder_load_failed"));
    CONTROL_CHECK(!controlErrorRetryable("MODEL_UNAVAILABLE", "model_missing"));
    CONTROL_CHECK(!controlErrorRetryable("MODEL_UNAVAILABLE", ""));
    CONTROL_CHECK(!controlErrorRetryable("INVALID_ARGUMENT", "option_unavailable"));
    {
        const auto line =
            controlErrorResponse({}, "BUSY", "request_limit", "x\xC3\xA9" + std::string(2000, 'y'), false);
        const auto value = parsed(line);
        const auto* data = errorData(value);
        CONTROL_CHECK(value.find("id")->isNull());
        CONTROL_CHECK(data->find("retryable")->asBoolean());
        CONTROL_CHECK(data->find("detail")->text().size() == 1024
                      && data->find("detail")->text().compare(0, 3, "x??") == 0);
        CONTROL_CHECK(!data->find("executed")->asBoolean());
    }
    CONTROL_CHECK(controlMethodCapability("session.info") == std::string("session.v1"));
    CONTROL_CHECK(controlMethodCapability("audio.set") == std::string("audio.device"));
    CONTROL_CHECK(controlMethodCapability("track.rename") == nullptr && controlMethodCapability("") == nullptr);

    // session.hello checks (PROTOCOL.md section 4.2), in their order.
    const std::string token(64, 'a');
    const auto verdict = [&token](const std::string& paramsJson) {
        std::string detail;
        const auto params = parseJson(paramsJson);
        return controlCheckHello(params.ok ? &params.value : nullptr, token, detail);
    };
    const std::string client = R"("client":{"name":"t","version":"1"})";
    using V = ControlHelloVerdict;
    CONTROL_CHECK(verdict("") == V::AuthFailed);
    CONTROL_CHECK(verdict("[]") == V::AuthFailed);
    CONTROL_CHECK(verdict(R"({"protocol":"1.0",)" + client + "}") == V::AuthFailed);
    CONTROL_CHECK(verdict(R"({"token":7,"protocol":"1.0",)" + client + "}") == V::AuthFailed);
    CONTROL_CHECK(verdict("{\"token\":\"" + std::string(64, 'b') + "\",\"protocol\":\"1.0\"," + client + "}")
                  == V::AuthFailed);
    CONTROL_CHECK(verdict("{\"token\":\"" + std::string(64, 'b') + "\",\"protocol\":\"99.0\"}") == V::AuthFailed);
    CONTROL_CHECK(verdict("{\"token\":\"" + token + "\",\"protocol\":\"1\"," + client + "}") == V::AuthFailed);
    CONTROL_CHECK(verdict("{\"token\":\"" + token + "\",\"protocol\":\"01.0\"," + client + "}") == V::AuthFailed);
    CONTROL_CHECK(verdict("{\"token\":\"" + token + "\",\"protocol\":2}") == V::AuthFailed);
    CONTROL_CHECK(verdict("{\"token\":\"" + token + "\",\"protocol\":\"2.0\",\"other\":1}") == V::ProtocolMismatch);
    CONTROL_CHECK(verdict("{\"token\":\"" + token + "\",\"protocol\":\"1.0\"}") == V::InvalidParams);
    CONTROL_CHECK(verdict("{\"token\":\"" + token + "\",\"protocol\":\"1.0\"," + client + ",\"x\":1}")
                  == V::InvalidParams);
    CONTROL_CHECK(
        verdict("{\"token\":\"" + token + "\",\"protocol\":\"1.0\",\"client\":{\"name\":\"a b\",\"version\":\"1\"}}")
        == V::InvalidParams);
    CONTROL_CHECK(verdict("{\"token\":\"" + token + "\",\"protocol\":\"1.7\"," + client + "}") == V::Accepted);
}

// --------------------------------------------------------------------------------------------------
// Discovery and ready files
// --------------------------------------------------------------------------------------------------

#if defined(_WIN32)
// True when the file's DACL is protected and allows only the process user (and optionally SYSTEM).
bool isPrivateToUser(const std::filesystem::path& path, std::string& why)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &dacl, nullptr, &descriptor)
        != ERROR_SUCCESS) {
        why = "cannot read the security descriptor";
        return false;
    }
    HANDLE token = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> userBuffer(size);
    GetTokenInformation(token, TokenUser, userBuffer.data(), size, &size);
    CloseHandle(token);
    const PSID user = reinterpret_cast<TOKEN_USER*>(userBuffer.data())->User.Sid;

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(descriptor, &control, &revision);
    bool ok = true;
    int userEntries = 0;
    if ((control & SE_DACL_PROTECTED) == 0) {
        why = "the DACL is not protected from inheritance";
        ok = false;
    } else if (dacl == nullptr) {
        why = "there is no DACL (everyone has access)";
        ok = false;
    }
    for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        GetAce(dacl, i, &ace);
        const auto* header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE) != 0) {
            why = "an entry is not an explicit allow entry";
            ok = false;
            break;
        }
        const PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
        if (EqualSid(sid, user))
            ++userEntries;
        else if (!IsWellKnownSid(sid, WinLocalSystemSid)) {
            why = "an entry grants access to an account other than the user and SYSTEM";
            ok = false;
        }
    }
    if (ok && userEntries == 0) {
        why = "no entry for the user";
        ok = false;
    }
    LocalFree(descriptor);
    return ok;
}
#else
bool isPrivateToUser(const std::filesystem::path& path, std::string& why)
{
    struct stat info
    {};
    if (::stat(path.c_str(), &info) != 0 || (info.st_mode & 0777) != 0600) {
        why = "the file mode is not 0600";
        return false;
    }
    return true;
}
#endif

void testDiscoveryDirectoryResolution()
{
    std::string error;
    ControlApiOptions options;
    options.enabled = true;
    const auto none = environmentOf({});
    CONTROL_CHECK(controlResolveDiscoveryDirectory(options,
                                                   environmentOf({{"LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local"}}),
                                                   ControlPlatform::Windows, error)
                  == "C:\\Users\\u\\AppData\\Local\\OpenTune\\control\\instances");
    CONTROL_CHECK(
        controlResolveDiscoveryDirectory(options, environmentOf({{"HOME", "/Users/u"}}), ControlPlatform::MacOS, error)
        == "/Users/u/Library/Application Support/OpenTune/control/instances");
    CONTROL_CHECK(controlResolveDiscoveryDirectory(options, environmentOf({{"HOME", "/home/u"}}),
                                                   ControlPlatform::OtherPosix, error)
                  == "/home/u/.local/state/OpenTune/control/instances");
    CONTROL_CHECK(controlResolveDiscoveryDirectory(options,
                                                   environmentOf({{"HOME", "/home/u"}, {"XDG_STATE_HOME", "/state"}}),
                                                   ControlPlatform::OtherPosix, error)
                  == "/state/OpenTune/control/instances");
    CONTROL_CHECK(controlResolveDiscoveryDirectory(options, none, ControlPlatform::Windows, error).empty()
                  && !error.empty());

    options.dataDirectory = "C:\\Data";
    CONTROL_CHECK(controlResolveDiscoveryDirectory(options, none, ControlPlatform::Windows, error)
                  == "C:\\Data\\control\\instances");
    options.discoveryDirectory = "C:\\Explicit";
    CONTROL_CHECK(controlResolveDiscoveryDirectory(options, none, ControlPlatform::Windows, error) == "C:\\Explicit");
}

void testDiscoveryFiles()
{
    CONTROL_CHECK(controlValidateBuildIdentity(testBuildIdentity()).empty());
    {
        auto identity = testBuildIdentity();
        identity.commit = "0123";
        CONTROL_CHECK(!controlValidateBuildIdentity(identity).empty());
        identity = testBuildIdentity();
        identity.sourceUrl = "http://example.invalid";
        CONTROL_CHECK(!controlValidateBuildIdentity(identity).empty());
        identity = testBuildIdentity();
        identity.appVersion.clear();
        CONTROL_CHECK(!controlValidateBuildIdentity(identity).empty());
    }

    const auto root = freshDirectory("discovery");
    const auto directory = root / "a" / "b" / "instances";
    ControlDiscovery discovery(pathString(directory));
    std::string error;
    CONTROL_CHECK(discovery.prepareDirectory(error));
    CONTROL_CHECK(std::filesystem::is_directory(directory));

    ControlDiscoveryRecord record;
    record.pid = controlCurrentProcessId();
    record.processStartTimeMs = controlProcessStartTimeMs();
    record.instanceId = SecureRandom::hex(16).value_or("");
    record.port = 4242;
    record.token = SecureRandom::hex(32).value_or("");
    record.serverProof = SecureRandom::hex(16).value_or("");
    record.launchNonce = std::string(18, 'n');
    record.build = testBuildIdentity();
    record.startedAtMs = controlNowMs();
    CONTROL_CHECK(record.processStartTimeMs > 0 && record.processStartTimeMs <= record.startedAtMs);
    CONTROL_CHECK(discovery.publish(record, error));

    const auto file = std::filesystem::u8path(discovery.filePath());
    CONTROL_CHECK(file.filename().string() == std::to_string(record.pid) + ".json");
    const auto content = readFile(file);
    CONTROL_CHECK(content.size() <= 8192);
    const auto json = parsed(content);
    // Exactly the members of the closed schema, in schema order.
    const char* const members[] = {"protocolMajors", "pid",         "processStartTimeMs", "instanceId", "port",
                                   "token",          "serverProof", "launchNonce",        "appVersion", "build",
                                   "buildTag",       "sourceUrl",   "startedAtMs",        "flavor"};
    CONTROL_CHECK(json.size() == std::size(members));
    for (std::size_t i = 0; i < json.size() && i < std::size(members); ++i)
        CONTROL_CHECK(json.keyAt(i) == members[i]);
    CONTROL_CHECK(toJsonString(*json.find("protocolMajors")) == "[1]");
    CONTROL_CHECK(json.find("pid")->asDouble() == static_cast<double>(record.pid));
    CONTROL_CHECK(json.find("token")->text() == record.token && json.find("token")->text().size() == 64);
    CONTROL_CHECK(json.find("serverProof")->text().size() == 32 && json.find("instanceId")->text().size() == 32);
    CONTROL_CHECK(json.find("launchNonce")->text() == *record.launchNonce);
    CONTROL_CHECK(toJsonString(*json.find("build"))
                  == R"({"commit":"0123456789abcdef0123456789abcdef01234567","tag":null})");
    CONTROL_CHECK(json.find("buildTag")->isNull() && json.find("flavor")->text() == "standalone");
    std::string why;
    if (!CONTROL_CHECK(isPrivateToUser(file, why)))
        note("discovery file: " + why);
    // Only the final file remains: no temporary file is left behind.
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(directory))
        ++entries;
    CONTROL_CHECK(entries == 1);

    const auto ready = root / "ready.json";
    CONTROL_CHECK(
        controlWritePrivateFile(pathString(ready), controlReadyFileContent(record.pid, record.instanceId), error));
    CONTROL_CHECK(readFile(ready)
                  == "{\"pid\":" + std::to_string(record.pid) + ",\"instanceId\":\"" + record.instanceId + "\"}");
    if (!CONTROL_CHECK(isPrivateToUser(ready, why)))
        note("ready file: " + why);
    // Replacing an existing file keeps the protection of the new file.
    CONTROL_CHECK(controlWritePrivateFile(pathString(ready), "{}", error) && readFile(ready) == "{}");
    CONTROL_CHECK(isPrivateToUser(ready, why));

    discovery.withdraw();
    CONTROL_CHECK(!std::filesystem::exists(file));

#if !defined(_WIN32)
    struct stat info
    {};
    CONTROL_CHECK(::stat(directory.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
    // A directory writable by others and a symbolic link are refused.
    const auto open = root / "open";
    std::filesystem::create_directories(open);
    ::chmod(open.c_str(), 0777);
    ControlDiscovery refused(pathString(open));
    CONTROL_CHECK(!refused.prepareDirectory(error));
    const auto link = root / "link";
    std::filesystem::create_directory_symlink(directory, link);
    ControlDiscovery viaLink(pathString(link));
    CONTROL_CHECK(!viaLink.prepareDirectory(error));
#endif
}

// --------------------------------------------------------------------------------------------------
// Server
// --------------------------------------------------------------------------------------------------

void testServerStartAndHello()
{
    ServerFixture fixture;
    if (!fixture.start("hello", {}, {}, false))
        return;
    const auto& discovery = fixture.discovery;
    CONTROL_CHECK(discovery.find("instanceId")->text() == fixture.server->instanceId());

    TestClient client;
    const auto hello = fixture.authenticate(client, "1");
    if (!CONTROL_CHECK(hello.has_value()))
        return;
    const auto& result = *hello->find("result");
    CONTROL_CHECK(result.find("protocol")->text() == "1.0");
    CONTROL_CHECK(result.find("instanceId")->text() == discovery.find("instanceId")->text());
    CONTROL_CHECK(result.find("pid")->asDouble() == discovery.find("pid")->asDouble());
    CONTROL_CHECK(result.find("serverProof")->text() == discovery.find("serverProof")->text());
    CONTROL_CHECK(result.find("appVersion")->text() == discovery.find("appVersion")->text());
    CONTROL_CHECK(toJsonString(*result.find("capabilities")) == R"(["session.v1"])");
    CONTROL_CHECK(toJsonString(*result.find("limits")) == toJsonString(controlLimitsToJson({})));
    CONTROL_CHECK(fixture.server->sessionCount() == 1);

    // Without a dispatcher every defined method other than session.hello is "not implemented".
    CONTROL_CHECK(client.sendLine(requestLine("2", "session.info")));
    CONTROL_CHECK(isError(client.readLine(), "UNSUPPORTED", "capability_missing"));
    CONTROL_CHECK(client.sendLine(requestLine("3", "project.get")));
    CONTROL_CHECK(isError(client.readLine(), "UNSUPPORTED", "capability_missing"));
    CONTROL_CHECK(client.sendLine(requestLine("4", "nope")));
    CONTROL_CHECK(isError(client.readLine(), "UNSUPPORTED", "unknown_method"));
    CONTROL_CHECK(client.staysQuiet(kOpenCheckMs));

    // The token and the server proof never reach the log.
    for (const auto& line : fixture.logLines())
        CONTROL_CHECK(line.find(fixture.token) == std::string::npos
                      && line.find(discovery.find("serverProof")->text()) == std::string::npos);
}

/** Expects the server to close the connection without sending a byte, within boundMs. */
bool closesSilently(TestClient& client, int boundMs)
{
    std::size_t bytes = 0;
    const bool closed = client.waitClosed(boundMs, bytes);
    return CONTROL_CHECK(closed) && CONTROL_CHECK(bytes == 0);
}

void testBeforeAuthentication()
{
    ServerFixture fixture;
    if (!fixture.start("preauth"))
        return;
    const auto port = fixture.port;

    // A browser's cross-protocol request: closed without a byte.
    {
        TestClient client;
        CONTROL_CHECK(client.connect(port));
        CONTROL_CHECK(client.sendRaw("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\n\r\n{}\n"));
        closesSilently(client, kPromptCloseMs);
    }
    // Other first lines that are not a session.hello request.
    const std::string notHello[] = {
        "[" + fixture.helloLine() + "]",
        R"({"jsonrpc":"2.0","method":"session.hello","params":{}})",
        requestLine("1", "session.info"),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session.hello\",\"params\":[]}",
        "{not json",
        "",
    };
    for (const auto& line : notHello) {
        TestClient client;
        CONTROL_CHECK(client.connect(port));
        CONTROL_CHECK(client.sendLine(line));
        closesSilently(client, kPromptCloseMs);
    }
    // 9 KiB without an LF: closed long before the hello timeout.
    {
        TestClient client;
        CONTROL_CHECK(client.connect(port));
        const auto started = Clock::now();
        CONTROL_CHECK(client.sendRaw(std::string(9 * 1024, 'a')));
        closesSilently(client, kPromptCloseMs);
        CONTROL_CHECK(millisecondsSince(started) < kHelloTimeoutLowerBoundMs);
    }
    // Slow drip: a valid hello sent one byte at a time never completes within helloTimeoutMs.
    {
        TestClient client;
        CONTROL_CHECK(client.connect(port));
        const auto started = Clock::now();
        const auto line = fixture.helloLine();
        std::size_t bytes = 0;
        bool closed = false;
        for (std::size_t i = 0; i < line.size() && !closed; ++i) {
            client.sendRaw(line.substr(i, 1));
            closed = client.waitClosed(kSlowDripIntervalMs, bytes);
        }
        const auto elapsed = millisecondsSince(started);
        CONTROL_CHECK(closed && bytes == 0);
        if (!CONTROL_CHECK(elapsed >= kHelloTimeoutLowerBoundMs && elapsed <= kHelloTimeoutUpperBoundMs))
            note("slow drip closed after " + std::to_string(elapsed) + " ms");
    }
    CONTROL_CHECK(waitUntil([&] { return fixture.server->pendingConnectionCount() == 0; }, kCounterSettleMs));
}

void testHelloFailures()
{
    ServerFixture fixture;
    if (!fixture.start("hello-failures"))
        return;
    const auto sendFirstLine = [&fixture](const std::string& line, std::optional<std::string>& response) {
        TestClient client;
        if (!client.connect(fixture.port) || !client.sendLine(line))
            return false;
        response = client.readLine();
        std::size_t bytes = 0;
        return client.waitClosed(kPromptCloseMs, bytes) && bytes == 0;
    };
    const std::string wrongToken = std::string(fixture.token).replace(0, 1, fixture.token[0] == 'a' ? "b" : "a");
    const std::string client = R"("client":{"name":"t","version":"1"})";
    std::optional<std::string> response;

    CONTROL_CHECK(sendFirstLine("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session.hello\",\"params\":{\"token\":\""
                                    + wrongToken + "\",\"protocol\":\"1.0\"," + client + "}}",
                                response));
    CONTROL_CHECK(response == std::optional<std::string>(kAuthFailedId1));
    CONTROL_CHECK(sendFirstLine(
        R"({"jsonrpc":"2.0","id":1,"method":"session.hello","params":{"token":"not-a-token","protocol":"1.0"}})",
        response));
    CONTROL_CHECK(response == std::optional<std::string>(kAuthFailedId1));
    CONTROL_CHECK(sendFirstLine(R"({"jsonrpc":"2.0","id":1e0,"method":"session.hello"})", response));
    CONTROL_CHECK(response == std::optional<std::string>(kAuthFailedId1));
    CONTROL_CHECK(sendFirstLine(fixture.helloLine("1", "99.0"), response));
    CONTROL_CHECK(response == std::optional<std::string>(kProtocolMismatchId1));
    CONTROL_CHECK(
        sendFirstLine("{\"jsonrpc\":\"2.0\",\"id\":\"h\",\"method\":\"session.hello\",\"params\":{\"token\":\""
                          + fixture.token + "\",\"protocol\":\"1.0\"}}",
                      response));
    CONTROL_CHECK(isError(response, "INVALID_ARGUMENT", "invalid_params") && responseIdText(response) == "\"h\"");
    CONTROL_CHECK(sendFirstLine("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session.hello\",\"params\":{\"token\":\""
                                    + fixture.token + "\",\"protocol\":\"1.0\"," + client + ",\"extra\":true}}",
                                response));
    CONTROL_CHECK(isError(response, "INVALID_ARGUMENT", "invalid_params"));
    CONTROL_CHECK(fixture.server->sessionCount() == 0);

    // A second hello on a session is refused and closes the connection.
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());
    CONTROL_CHECK(session.sendLine(fixture.helloLine("2")));
    CONTROL_CHECK(isError(session.readLine(), "INVALID_ARGUMENT", "hello_repeated"));
    closesSilently(session, kPromptCloseMs);
    CONTROL_CHECK(waitUntil([&] { return fixture.server->sessionCount() == 0; }, kCounterSettleMs));
}

void testConnectionLimits()
{
    ServerFixture fixture;
    if (!fixture.start("limits"))
        return;
    // Four sessions; the fifth hello gets BUSY(session_limit).
    std::vector<TestClient> sessions(4);
    for (auto& session : sessions)
        CONTROL_CHECK(fixture.authenticate(session).has_value());
    {
        TestClient fifth;
        CONTROL_CHECK(fifth.connect(fixture.port) && fifth.sendLine(fixture.helloLine("5")));
        const auto response = fifth.readLine();
        CONTROL_CHECK(isError(response, "BUSY", "session_limit"));
        CONTROL_CHECK(response && parsed(*response).find("error")->find("data")->find("retryable")->asBoolean());
        closesSilently(fifth, kPromptCloseMs);
    }
    sessions[0].close();
    CONTROL_CHECK(waitUntil([&] { return fixture.server->sessionCount() == 3; }, kCounterSettleMs));
    CONTROL_CHECK(fixture.authenticate(sessions[0]).has_value());

    // Eight silent connections wait for authentication; the ninth is closed at once.
    std::vector<TestClient> waiting(8);
    for (auto& client : waiting)
        CONTROL_CHECK(client.connect(fixture.port));
    CONTROL_CHECK(waitUntil([&] { return fixture.server->pendingConnectionCount() == 8; }, kCounterSettleMs));
    TestClient ninth;
    CONTROL_CHECK(ninth.connect(fixture.port));
    closesSilently(ninth, 1000);
    CONTROL_CHECK(waiting.front().staysQuiet(kOpenCheckMs));
    // The sessions still work.
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        CONTROL_CHECK(sessions[i].sendLine(requestLine(std::to_string(10 + i), "session.info")));
        CONTROL_CHECK(responseIdText(sessions[i].readLine()) == std::to_string(10 + i));
    }
}

void testFloodDoesNotDisturbSessions()
{
    ServerFixture fixture;
    if (!fixture.start("flood"))
        return;
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());

    std::vector<std::thread> flooders;
    for (int t = 0; t < kFloodThreads; ++t) {
        flooders.emplace_back([&fixture, t] {
            for (int i = 0; i < kFloodConnectionsPerThread; ++i) {
                TestClient client;
                if (!client.connect(fixture.port))
                    continue;
                if ((i + t) % 3 == 0)
                    client.sendRaw("GET / HTTP/1.1\r\n\r\n");
                else if ((i + t) % 3 == 1)
                    client.sendRaw(std::string(1000, 'x'));
                client.close();
            }
        });
    }
    long long slowest = 0;
    for (int i = 0; i < 20; ++i) {
        const auto started = Clock::now();
        CONTROL_CHECK(session.sendLine(requestLine(std::to_string(i), "session.info")));
        CONTROL_CHECK(responseIdText(session.readLine(kFloodResponseBoundMs)) == std::to_string(i));
        slowest = std::max(slowest, millisecondsSince(started));
    }
    for (auto& flooder : flooders)
        flooder.join();
    note("slowest session response during the flood: " + std::to_string(slowest) + " ms");
    CONTROL_CHECK(slowest < kFloodResponseBoundMs);
    CONTROL_CHECK(fixture.server->sessionCount() == 1);
    CONTROL_CHECK(waitUntil([&] { return fixture.server->pendingConnectionCount() == 0; }, kCounterSettleMs));
}

void testInFlightRules()
{
    ServerFixture fixture;
    if (!fixture.start("in-flight", {}, {"session.v1", "jobs.v1"}))
        return;
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());

    // A request whose id equals one in flight gets duplicate_id with id null; 7.0 equals 7, "7" does not.
    CONTROL_CHECK(session.sendLine(requestLine("7", "job.get")));
    CONTROL_CHECK(fixture.handler->waitForHeld(1));
    CONTROL_CHECK(session.sendRaw("{\"jsonrpc\":\"2.0\",\"id\":7.0,\"method\":\"session.info\",\"params\":{}}\n"));
    const auto duplicate = session.readLine();
    CONTROL_CHECK(isError(duplicate, "INVALID_ARGUMENT", "duplicate_id") && responseIdText(duplicate) == "null");
    CONTROL_CHECK(session.sendLine(requestLine("\"7\"", "session.info")));
    CONTROL_CHECK(responseIdText(session.readLine()) == "\"7\"");
    fixture.handler->releaseAll();
    CONTROL_CHECK(responseIdText(session.readLine()) == "7");

    // At most maxInFlightPerSession requests in flight; the next gets BUSY(request_limit), not executed.
    const int limit = ControlLimits{}.maxInFlightPerSession;
    for (int i = 0; i < limit; ++i)
        CONTROL_CHECK(session.sendLine(requestLine(std::to_string(100 + i), "job.get")));
    CONTROL_CHECK(fixture.handler->waitForHeld(static_cast<std::size_t>(limit)));
    CONTROL_CHECK(session.sendLine(requestLine("200", "session.info")));
    const auto busy = session.readLine();
    CONTROL_CHECK(isError(busy, "BUSY", "request_limit") && responseIdText(busy) == "200");
    // Completing from another thread (in OpenTune the message thread) never waits for the network.
    const auto releaseStarted = Clock::now();
    fixture.handler->releaseAll();
    CONTROL_CHECK(millisecondsSince(releaseStarted) < kCompletionBoundMs);
    std::set<std::string> answered;
    for (int i = 0; i < limit; ++i)
        answered.insert(responseIdText(session.readLine()));
    CONTROL_CHECK(answered.size() == static_cast<std::size_t>(limit) && answered.count("100") == 1);

    // A client that disconnects with a request in flight: the late response is discarded.
    {
        TestClient leaving;
        CONTROL_CHECK(fixture.authenticate(leaving).has_value());
        CONTROL_CHECK(leaving.sendLine(requestLine("1", "job.get")));
        CONTROL_CHECK(fixture.handler->waitForHeld(1));
        leaving.close();
        CONTROL_CHECK(waitUntil([&] { return fixture.server->sessionCount() == 1; }, kCounterSettleMs));
        fixture.handler->releaseAll();
    }
    // A client that disconnects in the middle of a line before authenticating.
    {
        TestClient leaving;
        CONTROL_CHECK(leaving.connect(fixture.port) && leaving.sendRaw("{\"jsonrpc\":\"2.0\",\"id\":1,"));
        leaving.close();
        CONTROL_CHECK(waitUntil([&] { return fixture.server->pendingConnectionCount() == 0; }, kCounterSettleMs));
    }
    CONTROL_CHECK(session.sendLine(requestLine("300", "session.info")));
    CONTROL_CHECK(responseIdText(session.readLine()) == "300");
}

/** The stand-in answers job.get for the jobs it knows and, as the dispatcher does, NOT_FOUND(job_not_found) for an id
    that no job has. */
void testStandInJobGet()
{
    ServerFixture fixture;
    if (!fixture.start("stand-in-job-get", {}, {"session.v1", "jobs.v1"}))
        return;
    fixture.handler->answerJobGetAfterWait = true;
    fixture.handler->knownJobIds = {"job-0"};
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());

    CONTROL_CHECK(session.sendLine(requestLine("1", "job.get", R"({"jobId":"job-0","waitMs":0})")));
    const auto known = session.readLine();
    const auto parsedKnown = known ? parseJson(*known) : JsonParseResult{};
    CONTROL_CHECK(parsedKnown.ok && parsedKnown.value.find("result") != nullptr
                  && parsedKnown.value.find("result")->find("job")->find("jobId")->text() == "job-0");

    // An id that no job has; the unbound ids of the vectors are of this kind.
    CONTROL_CHECK(session.sendLine(requestLine("2", "job.get", R"({"jobId":"j:00000000:999999","waitMs":0})")));
    const auto unknown = session.readLine();
    CONTROL_CHECK(isError(unknown, "NOT_FOUND", "job_not_found") && responseIdText(unknown) == "2");
    const auto parsedUnknown = unknown ? parseJson(*unknown) : JsonParseResult{};
    const JsonValue* data = parsedUnknown.ok ? errorData(parsedUnknown.value) : nullptr;
    CONTROL_CHECK(data != nullptr && data->find("executed") != nullptr && !data->find("executed")->asBoolean());
}

void testLineLimits()
{
    ServerFixture fixture;
    if (!fixture.start("lines"))
        return;
    const auto maxLine = static_cast<std::size_t>(ControlLimits{}.maxLineBytes);
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());
    // A line of exactly maxLineBytes is read (and is a parse error); the session stays open.
    CONTROL_CHECK(session.sendRaw(std::string(maxLine, 'a') + "\n"));
    CONTROL_CHECK(isError(session.readLine(), "INVALID_ARGUMENT", "parse_error"));
    CONTROL_CHECK(session.sendLine(requestLine("1", "session.info")));
    CONTROL_CHECK(responseIdText(session.readLine()) == "1");
    // One byte more without an LF: line_too_long with id null, then the connection closes.
    CONTROL_CHECK(session.sendRaw(std::string(maxLine + 1, 'a')));
    const auto tooLong = session.readLine();
    CONTROL_CHECK(isError(tooLong, "INVALID_ARGUMENT", "line_too_long") && responseIdText(tooLong) == "null");
    closesSilently(session, kPromptCloseMs);
}

void testIdleTimeout()
{
    ControlLimits limits;
    limits.idleTimeoutMs = kTestIdleTimeoutMs;
    ServerFixture fixture;
    if (!fixture.start("idle", limits, {"session.v1", "jobs.v1"}))
        return;
    {
        TestClient session;
        CONTROL_CHECK(fixture.authenticate(session).has_value());
        const auto started = Clock::now();
        closesSilently(session, kTestIdleTimeoutMs * 4);
        CONTROL_CHECK(millisecondsSince(started) >= kTestIdleTimeoutMs - 50);
    }
    // Not idle while a request is in flight.
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());
    CONTROL_CHECK(session.sendLine(requestLine("1", "job.get")));
    CONTROL_CHECK(fixture.handler->waitForHeld(1));
    CONTROL_CHECK(session.staysQuiet(kTestIdleTimeoutMs * 2));
    fixture.handler->releaseAll();
    CONTROL_CHECK(responseIdText(session.readLine()) == "1");
    closesSilently(session, kTestIdleTimeoutMs * 4);
}

void testStopWithBusyConnections()
{
    ServerFixture fixture;
    if (!fixture.start("stop", {}, {"session.v1", "jobs.v1", "read.v1"}))
        return;
    // Three sessions with a request in flight, one session that never reads its large responses, and
    // four connections that never authenticate.
    std::vector<TestClient> sessions(4);
    for (std::size_t i = 0; i < 3; ++i) {
        CONTROL_CHECK(fixture.authenticate(sessions[i]).has_value());
        CONTROL_CHECK(sessions[i].sendLine(requestLine("1", "job.get")));
    }
    CONTROL_CHECK(fixture.handler->waitForHeld(3));
    CONTROL_CHECK(fixture.authenticate(sessions[3]).has_value());
    for (int i = 0; i < 12; ++i)
        CONTROL_CHECK(sessions[3].sendLine(requestLine(std::to_string(i), "project.get")));
    std::vector<TestClient> silent(4);
    for (auto& client : silent)
        CONTROL_CHECK(client.connect(fixture.port));
    CONTROL_CHECK(waitUntil([&] { return fixture.server->pendingConnectionCount() == 4; }, kCounterSettleMs));
    sleepMs(200);

    const auto discoveryFile = std::filesystem::u8path(fixture.server->discoveryFilePath());
    CONTROL_CHECK(std::filesystem::exists(discoveryFile));
    const auto started = Clock::now();
    fixture.server->stop();
    const auto elapsed = millisecondsSince(started);
    note("stop() with busy connections took " + std::to_string(elapsed) + " ms");
    CONTROL_CHECK(elapsed < kStopBoundMs);
    CONTROL_CHECK(!fixture.server->isRunning());
    CONTROL_CHECK(!std::filesystem::exists(discoveryFile));
    CONTROL_CHECK(fixture.handler->stoppingCalls == 1);
    CONTROL_CHECK(fixture.server->sessionCount() == 0 && fixture.server->pendingConnectionCount() == 0);
    for (auto& client : silent) {
        std::size_t bytes = 0;
        CONTROL_CHECK(client.waitClosed(kPromptCloseMs, bytes) && bytes == 0);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        std::size_t bytes = 0;
        CONTROL_CHECK(sessions[i].waitClosed(kPromptCloseMs, bytes));
    }
    // Responses after the stop are discarded; stop() is idempotent; a stopped server cannot restart.
    fixture.handler->releaseAll();
    fixture.server->stop();
    std::string error;
    CONTROL_CHECK(!fixture.server->start(error));
    TestClient late;
    CONTROL_CHECK(!late.connect(fixture.port) || closesSilently(late, kPromptCloseMs));
}

void testStopAnswersBeforeClosing()
{
    ServerFixture fixture;
    if (!fixture.start("stop-order", {}, {"session.v1", "jobs.v1"}))
        return;
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());
    CONTROL_CHECK(session.sendLine(requestLine("1", "job.get")));
    if (!CONTROL_CHECK(fixture.handler->waitForHeld(1)))
        return;
    // Inside serverStopping(): wait longer than two poll intervals of the server, send a new request in
    // the stop window, then end the waiting job.get with SHUTTING_DOWN.
    std::optional<std::string> inWindow;
    fixture.handler->onStopping = [&session, &inWindow] {
        sleepMs(kStopWindowWaitMs);
        if (session.sendLine(requestLine("2", "session.info")))
            inWindow = session.readLine(kPromptCloseMs);
    };
    fixture.handler->endHeldOnStop = true;
    const auto started = Clock::now();
    fixture.server->stop();
    const auto elapsed = millisecondsSince(started);
    CONTROL_CHECK(isError(inWindow, "SHUTTING_DOWN", "") && responseIdText(inWindow) == "2");
    // The response that serverStopping() sent arrives, and only after it does the connection close.
    const auto waited = session.readLine(kPromptCloseMs);
    CONTROL_CHECK(isError(waited, "SHUTTING_DOWN", "") && responseIdText(waited) == "1");
    std::size_t bytes = 0;
    CONTROL_CHECK(session.waitClosed(kPromptCloseMs, bytes) && bytes == 0);
    CONTROL_CHECK(elapsed < kStopWindowWaitMs + kStopBoundMs);
    CONTROL_CHECK(fixture.handler->stoppingCalls == 1 && fixture.server->sessionCount() == 0);
}

void testStopAnswersArrivedRequests()
{
    ServerFixture fixture;
    if (!fixture.start("stop-arrived", {}, {"session.v1", "read.v1"}))
        return;
    fixture.handler->endHeldOnStop = true;
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());
    // transport.get keeps the connection thread inside the handler, so that request 2 arrives but is
    // not read until the sessions close.
    CONTROL_CHECK(session.sendLine(requestLine("1", "transport.get")));
    if (!CONTROL_CHECK(fixture.handler->waitForBlocked()))
        return;
    CONTROL_CHECK(session.sendLine(requestLine("2", "session.info")));
    sleepMs(kLoopbackSettleMs);
    std::thread stopper([&fixture] { fixture.server->stop(); });
    // serverStopping() ends request 1; the sessions close as soon as it returns.
    CONTROL_CHECK(waitUntil([&fixture] { return fixture.handler->stoppingCalls == 1; }, kReadTimeoutMs));
    sleepMs(kLoopbackSettleMs);
    fixture.handler->unblock();
    stopper.join();
    const auto first = session.readLine(kPromptCloseMs);
    CONTROL_CHECK(isError(first, "SHUTTING_DOWN", "") && responseIdText(first) == "1");
    const auto second = session.readLine(kPromptCloseMs);
    CONTROL_CHECK(isError(second, "SHUTTING_DOWN", "") && responseIdText(second) == "2");
    std::size_t bytes = 0;
    CONTROL_CHECK(session.waitClosed(kPromptCloseMs, bytes) && bytes == 0);
}

void testConnectionExceptionClosesChannel()
{
    ServerFixture fixture;
    if (!fixture.start("connection-exception", {}, {"session.v1", "jobs.v1"}))
        return;
    TestClient session;
    CONTROL_CHECK(fixture.authenticate(session).has_value());
    CONTROL_CHECK(session.sendLine(requestLine("1", "job.get")));
    if (!CONTROL_CHECK(fixture.handler->waitForHeld(1)))
        return;

    // A line shorter than maxLineBytes and without an LF: the server keeps reading it, and its growing
    // read buffer meets the injected allocation failure, which ends run() with an exception.
    const std::string partialLine(kInjectedLineBytes, 'a');
    failAllocationAtLeast = kInjectedFailureBytes;
    session.sendRaw(partialLine); // fails once the server has dropped the connection
    CONTROL_CHECK(waitUntil([&fixture] { return fixture.server->sessionCount() == 0; }, kCounterSettleMs));
    CONTROL_CHECK(failAllocationAtLeast.exchange(0) == 0);
    std::size_t bytes = 0;
    CONTROL_CHECK(session.waitClosed(kPromptCloseMs, bytes) && bytes == 0);
    const auto lines = fixture.logLines();
    CONTROL_CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("unexpected error") != std::string::npos;
    }));

    // The connection thread has ended, so its socket no longer exists. Completing the request still in
    // flight must not reach that socket and must not block: the channel is closed and the response is
    // discarded.
    sleepMs(kThreadExitSettleMs);
    const auto releaseStarted = Clock::now();
    fixture.handler->releaseAll();
    CONTROL_CHECK(millisecondsSince(releaseStarted) < kCompletionBoundMs);

    // The server still serves new sessions.
    TestClient next;
    CONTROL_CHECK(fixture.authenticate(next).has_value());
    CONTROL_CHECK(next.sendLine(requestLine("2", "session.info")));
    CONTROL_CHECK(responseIdText(next.readLine()) == "2");
}

void testStartFailures()
{
    const auto root = freshDirectory("start-failure");
    // The discovery directory cannot be created because a file is in the way.
    {
        std::ofstream(root / "file") << "x";
        ControlServerConfig config;
        config.discoveryDirectory = pathString(root / "file" / "instances");
        config.build = testBuildIdentity();
        ControlServer server(config);
        std::string error;
        CONTROL_CHECK(!server.start(error) && !error.empty());
        CONTROL_CHECK(!server.isRunning() && server.port() == 0);
    }
    // An identity that the discovery schema would reject.
    {
        ControlServerConfig config;
        config.discoveryDirectory = pathString(root / "instances");
        config.build = testBuildIdentity();
        config.build.commit = "not-a-commit";
        ControlServer server(config);
        std::string error;
        CONTROL_CHECK(!server.start(error) && !error.empty());
        CONTROL_CHECK(!std::filesystem::exists(root / "instances"));
    }
    // The ready file is written after the discovery file; a ready-file failure is only logged.
    {
        ControlServerConfig config;
        config.discoveryDirectory = pathString(root / "instances");
        config.readyFile = pathString(root / "ready.json");
        config.build = testBuildIdentity();
        ControlServer server(config);
        std::string error;
        CONTROL_CHECK(server.start(error));
        const auto ready = parsed(readFile(root / "ready.json"));
        CONTROL_CHECK(ready.find("instanceId")->text() == server.instanceId() && ready.size() == 2);
        server.stop();
        CONTROL_CHECK(!std::filesystem::exists(std::filesystem::u8path(server.discoveryFilePath())));

        config.readyFile = pathString(root / "missing" / "ready.json");
        ControlServer second(config);
        CONTROL_CHECK(second.start(error) && second.isRunning());
    }
}

// --------------------------------------------------------------------------------------------------
// Content handles, revision tokens and cursors
// --------------------------------------------------------------------------------------------------

/** The patterns of common.schema.json: contentId, placementId and revisionToken. */
bool matchesHandlePattern(const std::string& text, char prefix)
{
    if (text.size() < 14 || text[0] != prefix || text[1] != ':' || text[10] != ':')
        return false;
    for (std::size_t i = 2; i < 10; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    const auto rest = text.substr(11);
    const auto colon = rest.find(':');
    auto decimal = [](const std::string& part) {
        return !part.empty() && part.find_first_not_of("0123456789") == std::string::npos
               && (part.size() == 1 || part[0] != '0');
    };
    return colon != std::string::npos && decimal(rest.substr(0, colon)) && decimal(rest.substr(colon + 1));
}

bool matchesRevisionTokenPattern(const std::string& text)
{
    return !text.empty() && text.size() <= 128
           && text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-")
                  == std::string::npos;
}

void writeBinaryFile(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/** 32 hexadecimal digits made at run time, so that no literal in the sources looks like a key. */
std::string fakeSecret(unsigned seed)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < 32; ++i)
        out.push_back(digits[(seed + i * 5 + i / 3) % 16]);
    return out;
}

/** The redaction of log lines and the tail of a log file (app.getLog). */
void testLogTail()
{
    const std::string token = fakeSecret(3);
    const std::string proof = fakeSecret(11);
    CONTROL_CHECK(token != proof);
    std::string upperToken = token;
    for (auto& c : upperToken)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const std::vector<std::string> secrets{token, proof};
    const std::string home = "C:\\Users\\someone";
    const auto spellings = controlHomeDirectorySpellings(home);
    CONTROL_CHECK(
        (spellings == std::vector<std::string>{"C:\\\\Users\\\\someone", "C:\\Users\\someone", "C:/Users/someone"}));
    // A root or nothing is not a home directory: replacing it would mangle every path.
    CONTROL_CHECK(controlHomeDirectorySpellings("").empty() && controlHomeDirectorySpellings("/").empty());
    CONTROL_CHECK((controlHomeDirectorySpellings("/home/someone/")
                   == std::vector<std::string>{"\\\\home\\\\someone", "/home/someone", "\\home\\someone"}));

    // Secrets and the home directory in every spelling, also upper case and several times in a line.
    CONTROL_CHECK(controlRedactLogLine("token=" + token + " proof=" + proof, secrets, spellings)
                  == "token=<redacted> proof=<redacted>");
    CONTROL_CHECK(controlRedactLogLine("TOKEN=" + upperToken + " " + token, secrets, spellings)
                  == "TOKEN=<redacted> <redacted>");
    CONTROL_CHECK(controlRedactLogLine("open C:\\Users\\someone\\a.wav and c:/users/someone/b.wav and "
                                       "{\"p\":\"C:\\\\Users\\\\someone\\\\c.wav\"}",
                                       secrets, spellings)
                  == "open ~\\a.wav and ~/b.wav and {\"p\":\"~\\\\c.wav\"}");
    CONTROL_CHECK(controlRedactLogLine("nothing to see", secrets, spellings) == "nothing to see");
    // A short secret would match ordinary text and is ignored.
    CONTROL_CHECK(controlRedactLogLine("abc abc", {"abc"}, {}) == "abc abc");
    // Invalid UTF-8 becomes '?'; valid multi-byte text stays.
    CONTROL_CHECK(controlRedactLogLine(std::string("a\xff") + "b\xc3\xa9\xe2\x82", {}, {}) == "a?b\xc3\xa9??");
    // The cut comes after the replacement, at a code point: a secret at the cut never leaves its start.
    const auto longLine =
        std::string(4090, 'x') + token + "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
    const auto cut = controlRedactLogLine(longLine, secrets, spellings);
    CONTROL_CHECK(utf8CodePointCount(cut) == kControlLogLineMaxCodePoints && isValidUtf8(cut));
    CONTROL_CHECK(cut == std::string(4090, 'x') + "<redac");

    // The tail of a file.
    const auto directory = freshDirectory("log-tail");
    const auto file = directory / "OpenTune.log";
    CONTROL_CHECK(!controlReadLogTail(directory / "missing.log", 10, secrets, spellings).readable);
    writeBinaryFile(file, "");
    auto tail = controlReadLogTail(file, 10, secrets, spellings);
    CONTROL_CHECK(tail.readable && tail.lines.empty() && !tail.truncated);
    // CRLF and LF line ends, a blank line, a last line that is still being written, redaction inside.
    writeBinaryFile(file, "one\r\ntwo " + token + "\n\nthree C:\\Users\\someone\\x\r\nfour");
    tail = controlReadLogTail(file, 10, secrets, spellings);
    CONTROL_CHECK(tail.readable && !tail.truncated);
    CONTROL_CHECK((tail.lines == std::vector<std::string>{"one", "two <redacted>", "", "three ~\\x", "four"}));
    tail = controlReadLogTail(file, 2, secrets, spellings);
    CONTROL_CHECK(tail.truncated && (tail.lines == std::vector<std::string>{"three ~\\x", "four"}));
    // A final line end does not make an empty line.
    writeBinaryFile(file, "a\nb\n");
    tail = controlReadLogTail(file, 5, secrets, spellings);
    CONTROL_CHECK(!tail.truncated && (tail.lines == std::vector<std::string>{"a", "b"}));

    // A file longer than the window: only whole lines from its end, however long the file is.
    std::string big;
    constexpr std::size_t kLineBytes = 100;
    for (int i = 0; big.size() < kControlLogTailWindowBytes + 5 * kLineBytes + 37; ++i) {
        std::string line = "L" + std::to_string(1000000 + i) + " ";
        line.resize(kLineBytes - 1, '.');
        big += line + "\n";
    }
    writeBinaryFile(file, big);
    tail = controlReadLogTail(file, 1000, secrets, spellings);
    CONTROL_CHECK(tail.truncated && tail.lines.size() == 1000);
    bool whole = true;
    for (const auto& line : tail.lines)
        whole = whole && line.size() == kLineBytes - 1 && line[0] == 'L';
    CONTROL_CHECK(whole);
    CONTROL_CHECK(
        tail.lines.back().rfind("L" + std::to_string(1000000 + static_cast<int>(big.size() / kLineBytes) - 1), 0) == 0);
    // Asking for more lines than the window holds returns the window's whole lines and says it is cut.
    tail = controlReadLogTail(file, 1000000, secrets, spellings);
    CONTROL_CHECK(tail.truncated && tail.lines.size() >= kControlLogTailWindowBytes / kLineBytes - 1
                  && tail.lines.size() <= kControlLogTailWindowBytes / kLineBytes + 1);
    whole = true;
    for (const auto& line : tail.lines)
        whole = whole && line.size() == kLineBytes - 1 && line[0] == 'L';
    CONTROL_CHECK(whole);
}

void testHandles()
{
    // Format and parse round trip; the forms follow the schema patterns.
    const std::string contentId = controlFormatHandle(ControlHandleKind::Content, "0123abcd", 2, 17);
    CONTROL_CHECK(contentId == "c:0123abcd:2:17" && matchesHandlePattern(contentId, 'c'));
    const std::string placementId = controlFormatHandle(ControlHandleKind::Placement, "0123abcd", 0, 3);
    CONTROL_CHECK(placementId == "p:0123abcd:0:3" && matchesHandlePattern(placementId, 'p'));
    const auto parsedContent = controlParseHandle(ControlHandleKind::Content, contentId);
    if (CONTROL_CHECK(parsedContent.has_value())) {
        CONTROL_CHECK(parsedContent->instanceShort == "0123abcd" && parsedContent->epoch == 2
                      && parsedContent->object == 17);
        CONTROL_CHECK(!parsedContent->epochOverflow && !parsedContent->objectOverflow);
    }
    CONTROL_CHECK(controlInstanceShort("0123abcd456789ef0123456789abcdef") == "0123abcd");

    // A handle of one kind never passes the form check of the other (PROTOCOL.md section 9 rule 1).
    CONTROL_CHECK(!controlParseHandle(ControlHandleKind::Content, placementId));
    CONTROL_CHECK(!controlParseHandle(ControlHandleKind::Placement, contentId));
    for (const char* invalid :
         {"", "c:", "c:0123abcd:1", "c:0123abcd:1:", "c:0123abcd::1", "c:0123ABCD:1:1", "c:0123abc:1:1",
          "c:0123abcde:1:1", "c:0123abcd:01:1", "c:0123abcd:1:01", "c:0123abcd:1:1:1", "c:0123abcd:-1:1",
          "c:0123abcd:1:1 ", "x:0123abcd:1:1", "j:0123abcd:1", "u:0123abcd:1"})
        CONTROL_CHECK(!controlParseHandle(ControlHandleKind::Content, invalid));

    // Numbers beyond 64 bits keep the valid form and are flagged, so resolution reports them as stale
    // (epoch) or unknown (object) instead of rejecting the form.
    const auto huge = controlParseHandle(ControlHandleKind::Content, "c:0123abcd:99999999999999999999:1");
    CONTROL_CHECK(huge && huge->epochOverflow && !huge->objectOverflow);
    const auto hugeObject = controlParseHandle(ControlHandleKind::Content, "c:0123abcd:0:18446744073709551616");
    CONTROL_CHECK(hugeObject && hugeObject->objectOverflow && !hugeObject->epochOverflow);
    const auto maxObject = controlParseHandle(ControlHandleKind::Content, "c:0123abcd:0:18446744073709551615");
    CONTROL_CHECK(maxObject && !maxObject->objectOverflow && maxObject->object == 18446744073709551615ull);

    // Tokens: equal fields give equal tokens, the order and the boundaries of fields count, -0.0 is 0.0.
    const auto token = [](double value, const char* text) {
        return ControlTokenHasher('r').add(std::uint64_t{7}).add(value).add(std::string_view(text)).token();
    };
    CONTROL_CHECK(token(1.5, "a") == token(1.5, "a"));
    CONTROL_CHECK(token(0.0, "a") == token(-0.0, "a"));
    CONTROL_CHECK(token(1.5, "a") != token(1.5, "b") && token(1.5, "a") != token(2.5, "a"));
    CONTROL_CHECK(ControlTokenHasher('x').add(std::string_view("ab")).add(std::string_view("c")).token()
                  != ControlTokenHasher('x').add(std::string_view("a")).add(std::string_view("bc")).token());
    CONTROL_CHECK(ControlTokenHasher('a').token() != ControlTokenHasher('b').token());
    const auto sample = token(1.5, "a");
    CONTROL_CHECK(sample.size() == 17 && sample[0] == 'r' && matchesRevisionTokenPattern(sample));

    // Cursors: round trip, kind of the issuing method, bounded form.
    const std::string paramsHash = ControlTokenHasher('q').add(std::string_view("params")).token();
    const std::string tokensHash = ControlTokenHasher('t').add(std::string_view("tokens")).token();
    const std::string cursor = controlFormatCursor({'n', 200, paramsHash, tokensHash});
    CONTROL_CHECK(cursor.size() <= 512);
    const auto parsedCursor = controlParseCursor('n', cursor);
    if (CONTROL_CHECK(parsedCursor.has_value()))
        CONTROL_CHECK(parsedCursor->offset == 200 && parsedCursor->paramsHash == paramsHash
                      && parsedCursor->tokensHash == tokensHash);
    CONTROL_CHECK(!controlParseCursor('p', cursor));
    for (const std::string& invalid :
         {std::string("n1"), std::string("n1."), "n2.0." + paramsHash + "." + tokensHash,
          "n1.01." + paramsHash + "." + tokensHash, "n1.0." + paramsHash, "n1.0." + paramsHash + ".t123",
          "n1.0." + paramsHash + "." + tokensHash + "." + tokensHash,
          "n1.99999999999999999999." + paramsHash + "." + tokensHash})
        CONTROL_CHECK(!controlParseCursor('n', invalid));
}

// --------------------------------------------------------------------------------------------------
// Session vectors of the specification copy
// --------------------------------------------------------------------------------------------------

/**
 * Runs one vector of Tests/Control/spec/otcp/v1/vectors (format: vectors/README.md) against a fresh
 * server. Implements the steps, the matchers and the plain-integer check of response ids.
 */
class VectorRunner
{
public:
    explicit VectorRunner(const JsonValue& vector) : vector_(vector) {}

    /** Empty on success, otherwise what went wrong. */
    std::string run()
    {
        // A vector of jobs.v1 here tests the channel (duplicate ids, the in-flight limit) with waiting job.get
        // requests; the stand-in handler reports the capability and answers a waiting job.get after its waitMs.
        const bool jobsVector = vector_.find("capability")->text() == "jobs.v1";
        // The stand-in reports every capability the vector needs (the main one and the extra ones that
        // skipReason lets through), so that what is let through and what is reported agree.
        std::vector<std::string> capabilities = {"session.v1"};
        const auto need = [&capabilities](const std::string& capability) {
            if (std::find(capabilities.begin(), capabilities.end(), capability) == capabilities.end())
                capabilities.push_back(capability);
        };
        need(vector_.find("capability")->text());
        if (const JsonValue* extra = vector_.find("extraCapabilities"))
            for (std::size_t i = 0; i < extra->size(); ++i)
                need(extra->at(i).text());
        if (!fixture_.start("vector", {}, capabilities))
            return "the server did not start";
        fixture_.handler->answerJobGetAfterWait = jobsVector;
        if (const JsonValue* given = vector_.find("given"))
            if (const JsonValue* jobs = given->find("jobs"))
                for (std::size_t i = 0; i < jobs->size(); ++i) {
                    const auto id = "job-" + std::to_string(i);
                    matcher_.bindings[jobs->at(i).find("bind")->text()] = JsonValue::string(id);
                    fixture_.handler->knownJobIds.insert(id);
                }
        for (std::size_t i = 0; i < fixture_.discovery.size(); ++i)
            matcher_.bindings["discovery." + fixture_.discovery.keyAt(i)] = fixture_.discovery.at(i);
        std::string wrong;
        do {
            wrong = SecureRandom::hex(fixture_.token.size() / 2).value_or("");
        } while (wrong == fixture_.token);
        matcher_.bindings["runner.wrongToken"] = JsonValue::string(wrong);

        auto& main = connections_["main"];
        if (!main.connect(fixture_.port))
            return "cannot connect";
        if (vector_.find("session")->text() == "authenticated") {
            if (!main.sendLine(fixture_.helloLine("\"runner.hello\"")) || !isResult(main.readLine()))
                return "the runner's session.hello failed";
        }
        lastStepEnd_ = Clock::now();
        if (vector_.find("type")->text() == "request")
            return runRequest();
        const JsonValue& steps = *vector_.find("steps");
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const std::string failure = runStep(steps.at(i));
            if (!failure.empty())
                return "step " + std::to_string(i + 1) + ": " + failure;
            lastStepEnd_ = Clock::now();
        }
        return {};
    }

private:
    static bool isResult(const std::optional<std::string>& line)
    {
        return line && parseJson(*line).ok && parseJson(*line).value.find("result") != nullptr;
    }

    std::string runRequest()
    {
        auto& main = connections_["main"];
        std::string failure;
        const auto request = matcher_.substitute(*vector_.find("request"), failure);
        if (!failure.empty())
            return failure;
        if (!main.sendLine(toJsonString(request)))
            return "cannot send the request";
        failure = matcher_.expectLine(main, *vector_.find("response"));
        if (!failure.empty())
            return failure;
        std::size_t bytes = 0;
        if (vector_.find("connectionAfter")->text() == "closed")
            return main.waitClosed(kReadTimeoutMs, bytes) && bytes == 0 ? std::string() : "the connection stayed open";
        return main.staysQuiet(kOpenCheckMs) ? std::string() : "the connection closed or sent more";
    }

    std::string runStep(const JsonValue& step)
    {
        const JsonValue* connectionName = step.find("connection");
        auto& connection = connections_[connectionName != nullptr ? connectionName->text() : "main"];
        if (const JsonValue* name = step.find("connect"))
            return connections_[name->text()].connect(fixture_.port) ? std::string() : "cannot connect";
        if (const JsonValue* value = step.find("send")) {
            std::string failure;
            const auto resolved = matcher_.substitute(*value, failure);
            if (!failure.empty())
                return failure;
            return connection.sendLine(toJsonString(resolved)) ? std::string() : "send failed";
        }
        if (const JsonValue* text = step.find("sendRaw"))
            return connection.sendRaw(text->text()) ? std::string() : "send failed";
        if (const JsonValue* hex = step.find("sendHex")) {
            std::string bytes;
            for (std::size_t i = 0; i + 1 < hex->text().size(); i += 2)
                bytes += static_cast<char>(std::stoi(hex->text().substr(i, 2), nullptr, 16));
            return connection.sendRaw(bytes) ? std::string() : "send failed";
        }
        if (const JsonValue* filled = step.find("sendFilled")) {
            std::string bytes = filled->find("head")->text();
            const auto total = static_cast<std::size_t>(filled->find("bytes")->asDouble());
            bytes.append(total - bytes.size(), filled->find("fill")->text()[0]);
            bytes += filled->find("tail")->text();
            return connection.sendRaw(bytes) ? std::string() : "send failed";
        }
        if (const JsonValue* expected = step.find("expect"))
            return matcher_.expectLine(connection, *expected);
        if (const JsonValue* closed = step.find("expectClosed")) {
            const JsonValue* within = closed->find("withinMs");
            const int bound = within != nullptr ? static_cast<int>(within->asDouble()) : kReadTimeoutMs;
            const int remaining = std::max(0, bound - static_cast<int>(millisecondsSince(lastStepEnd_)));
            std::size_t bytes = 0;
            if (!connection.waitClosed(remaining, bytes))
                return "the connection did not close within " + std::to_string(bound) + " ms";
            const JsonValue* noBytes = closed->find("noBytes");
            if (noBytes != nullptr && noBytes->asBoolean() && bytes != 0)
                return "bytes arrived before the close";
            return {};
        }
        if (const JsonValue* pause = step.find("pauseMs")) {
            sleepMs(static_cast<int>(pause->asDouble()));
            return {};
        }
        return "unknown step";
    }

    const JsonValue& vector_;
    ServerFixture fixture_;
    VectorMatcher matcher_;
    std::map<std::string, TestClient> connections_;
    Clock::time_point lastStepEnd_;
};

/** Why a vector cannot run against this server, or empty when it can. The server here has a stand-in
    handler; ControlProcessorTests.Vectors runs the vectors of the methods against the real dispatcher. */
std::string skipReason(const std::string& id, const JsonValue& vector)
{
    const std::set<std::string> reported = {"session.v1", "jobs.v1"};
    if (reported.count(vector.find("capability")->text()) == 0)
        return "needs capability " + vector.find("capability")->text() + ", which the stand-in handler does not report";
    if (const JsonValue* extra = vector.find("extraCapabilities"))
        for (std::size_t i = 0; i < extra->size(); ++i)
            if (reported.count(extra->at(i).text()) == 0)
                return "needs capability " + extra->at(i).text();
    if (const JsonValue* given = vector.find("given"))
        for (std::size_t i = 0; i < given->size(); ++i)
            if (given->keyAt(i) != "withoutCapabilities"
                && !(given->keyAt(i) == "jobs" && vector.find("capability")->text() == "jobs.v1"))
                return "precondition given." + given->keyAt(i) + " needs the dispatcher";
    if (id == "session/info-ok")
        return "checks the session.info result of the dispatcher (ControlProcessorTests.Vectors runs it)";
    return {};
}

void testSessionVectors()
{
    const auto directory = std::filesystem::u8path(OPENTUNE_CONTROL_SPEC_DIR) / "vectors" / "session";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    CONTROL_CHECK(files.size() >= 20);
    int passed = 0;
    int skipped = 0;
    std::set<std::string> ranIds;
    VectorReport report("ControlUnitTests");
    for (const auto& file : files) {
        const auto vector = parsed(readFile(file));
        const std::string id = vector.find("id")->text();
        if (const auto reason = skipReason(id, vector); !reason.empty()) {
            ++skipped;
            report.skip(id, reason);
            note("SKIP " + id + ": " + reason);
            continue;
        }
        const auto started = Clock::now();
        const std::string failure = VectorRunner(vector).run();
        report.ran(id, failure);
        if (CONTROL_CHECK(failure.empty())) {
            ++passed;
            ranIds.insert(id);
        }
        note((failure.empty() ? "PASS " : "FAIL ") + id + " (" + std::to_string(millisecondsSince(started)) + " ms)"
             + (failure.empty() ? "" : ": " + failure));
    }
    report.write();
    // The channel rules only this runner can check (the dispatcher runners have no listener).
    for (const char* id : {"session/duplicate-id", "session/request-limit"})
        CONTROL_CHECK(ranIds.count(id) == 1);
    note(std::to_string(passed) + " session vectors passed, " + std::to_string(skipped) + " skipped");
}

/** The rule of the editor's bridge that takes the changed mark of a view switch back (ControlEditorBridgeViewSwitch.h):
    only for the commit of the transport bar's BPM field. The rule reads plain values, so it runs here without a window;
    ControlEditorBridge reads the values from the editor and calls it. */
void testViewSwitchSettle()
{
    // A clean project, tempo 120 in 4/4, and the dirty generation 7 before the switch.
    ControlViewSwitchBaseline baseline;
    baseline.dirty = false;
    baseline.dirtyGeneration = 7;
    baseline.bpm = 120.0;
    baseline.timeSignatureNumerator = 4;
    baseline.timeSignatureDenominator = 4;
    const auto takenBack = [&baseline](bool dirtyNow, std::uint64_t generationNow, double bpmNow, int numeratorNow,
                                       int denominatorNow) {
        return controlViewSwitchMarkIsBpmCommit(baseline, dirtyNow, generationNow, bpmNow, numeratorNow,
                                                denominatorNow);
    };

    // The focus was in the BPM field or in the view that the switch hides: the commit of the field marked a clean
    // project once, with the tempo and the time signature as they were, and the mark is taken back.
    baseline.focusLeadsToBpmCommit = true;
    CONTROL_CHECK(takenBack(true, 8, 120.0, 4, 4));
    // Nothing marked the project: nothing to take back.
    CONTROL_CHECK(!takenBack(false, 7, 120.0, 4, 4));
    // The tempo or the time signature changed: a real change (a tempo with more decimals that the field rewrote, a
    // value typed into the field), the mark stays.
    CONTROL_CHECK(!takenBack(true, 8, 120.3, 4, 4));
    CONTROL_CHECK(!takenBack(true, 8, 120.0, 3, 4));
    CONTROL_CHECK(!takenBack(true, 8, 120.0, 4, 8));
    // The mark was set twice: a second field committed with the BPM field (the focus was in a field inside the view
    // that the switch hides, say), and that commit is a real change that the BPM field's commit must not take back.
    CONTROL_CHECK(!takenBack(true, 9, 120.0, 4, 4));
    CONTROL_CHECK(!takenBack(true, 12, 120.0, 4, 4));
    // The project was marked and the generation did not move, or moved back: not what one commit does.
    CONTROL_CHECK(!takenBack(true, 7, 120.0, 4, 4));
    CONTROL_CHECK(!takenBack(true, 6, 120.0, 4, 4));
    // The project was changed before the switch: the mark is not the commit's.
    baseline.dirty = true;
    CONTROL_CHECK(!takenBack(true, 8, 120.0, 4, 4));

    // The focus was anywhere else (a field that commits when it loses the focus, whose commit leaves the tempo and the
    // time signature alone; no focus at all): the mark stays, with every other value as above.
    baseline.dirty = false;
    baseline.focusLeadsToBpmCommit = false;
    CONTROL_CHECK(!takenBack(true, 8, 120.0, 4, 4));
    CONTROL_CHECK(!takenBack(true, 8, 120.3, 4, 4));
    CONTROL_CHECK(!takenBack(false, 7, 120.0, 4, 4));
    CONTROL_CHECK(!takenBack(true, 9, 120.0, 4, 4));
    baseline.dirty = true;
    CONTROL_CHECK(!takenBack(true, 8, 120.0, 4, 4));

    // The baseline of an editor that has not been read takes nothing back.
    CONTROL_CHECK(!controlViewSwitchMarkIsBpmCommit(ControlViewSwitchBaseline{}, true, 1, 0.0, 0, 0));

    // The first mark of a session (generation 0 to 1) is a commit like any other: with the focus where the commit
    // follows it, it is taken back.
    ControlViewSwitchBaseline first;
    first.focusLeadsToBpmCommit = true;
    first.bpm = 120.0;
    first.timeSignatureNumerator = 4;
    first.timeSignatureDenominator = 4;
    CONTROL_CHECK(controlViewSwitchMarkIsBpmCommit(first, true, 1, 120.0, 4, 4));
}

} // namespace

// Optional argument: run only the tests whose name contains it.
int main(int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : "";
    const std::pair<const char*, void (*)()> tests[] = {
        {"options", testOptions},
        {"path policy", testPathPolicy},
        {"secure random", testSecureRandom},
        {"json", testJson},
        {"request ids", testRequestIds},
        {"protocol", testProtocol},
        {"discovery directory", testDiscoveryDirectoryResolution},
        {"discovery files", testDiscoveryFiles},
        {"server start and hello", testServerStartAndHello},
        {"before authentication", testBeforeAuthentication},
        {"hello failures", testHelloFailures},
        {"connection limits", testConnectionLimits},
        {"flood", testFloodDoesNotDisturbSessions},
        {"in-flight rules", testInFlightRules},
        {"stand-in job.get", testStandInJobGet},
        {"line limits", testLineLimits},
        {"idle timeout", testIdleTimeout},
        {"stop with busy connections", testStopWithBusyConnections},
        {"stop answers before closing", testStopAnswersBeforeClosing},
        {"stop answers arrived requests", testStopAnswersArrivedRequests},
        {"connection exception", testConnectionExceptionClosesChannel},
        {"start failures", testStartFailures},
        {"handles", testHandles},
        {"log tail", testLogTail},
        {"session vectors", testSessionVectors},
        {"view switch settle", testViewSwitchSettle},
    };
    if (!ControlSocket::acquireNetworking()) {
        std::cerr << "cannot initialise networking" << std::endl;
        return 1;
    }
    const auto started = Clock::now();
    for (const auto& [name, run] : tests) {
        if (std::string(name).find(filter) == std::string::npos)
            continue;
        const int before = failureCount();
        const auto testStarted = Clock::now();
        std::cout << "[ RUN  ] " << name << std::endl;
        run();
        std::cout << (failureCount() == before ? "[  OK  ] " : "[FAILED] ") << name << " ("
                  << millisecondsSince(testStarted) << " ms)" << std::endl;
    }
    ControlSocket::releaseNetworking();
    std::cout << (failureCount() == 0 ? "All control unit tests passed" : "Control unit tests FAILED") << " in "
              << millisecondsSince(started) << " ms" << std::endl;
    return failureCount() == 0 ? 0 : 1;
}
