// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlApiOptions.h"

#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace OpenTune {

namespace {

constexpr std::string_view kEnableArgument = "--control-api";
constexpr std::string_view kDirectoryPrefix = "--control-api-dir=";
constexpr std::string_view kReadyFilePrefix = "--control-api-ready-file=";
constexpr std::string_view kLaunchNoncePrefix = "--control-api-launch-nonce=";

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

bool isValidLaunchNonce(std::string_view nonce)
{
    if (nonce.size() < 16 || nonce.size() > 128)
        return false;
    for (const char c : nonce) {
        const bool ok =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

void addError(std::string& error, const std::string& message)
{
    if (!error.empty())
        error += "; ";
    error += message;
}

} // namespace

std::optional<std::string> controlSystemEnvironment(const char* name)
{
#if defined(_WIN32)
    const int wideNameLength = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (wideNameLength <= 0)
        return std::nullopt;
    std::wstring wideName(static_cast<std::size_t>(wideNameLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wideName.data(), wideNameLength);
    // The first call returns the size including the terminating null (1 for an empty value), or 0
    // when the variable is not set.
    const DWORD needed = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (needed == 0)
        return std::nullopt;
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(), needed);
    if (length >= needed)
        return std::nullopt;
    value.resize(length);
    if (value.empty())
        return std::string();
    const int utf8Length =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return std::nullopt;
    std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), utf8Length, nullptr,
                        nullptr);
    return utf8;
#else
    const char* value = std::getenv(name);
    if (value == nullptr)
        return std::nullopt;
    return std::string(value);
#endif
}

ControlApiOptions parseControlApiOptions(const std::vector<std::string>& args, const ControlEnvironmentLookup& env,
                                         ControlPathStyle style)
{
    ControlApiOptions options;
    const auto switchValue = env ? env("OPENTUNE_CONTROL_API") : std::nullopt;
    options.enabled = switchValue.has_value() && *switchValue == "1";
    for (const auto& arg : args)
        if (arg == kEnableArgument)
            options.enabled = true;
    options.dataDirectory = env ? env("OPENTUNE_DATA_DIR") : std::nullopt;
    if (!options.enabled)
        return options;

    auto takeValue = [&options](std::optional<std::string>& slot, std::string_view arg, std::string_view prefix) {
        if (slot.has_value())
            addError(options.error, std::string(prefix.substr(0, prefix.size() - 1)) + " is given more than once");
        else
            slot = std::string(arg.substr(prefix.size()));
    };
    for (const auto& arg : args) {
        const std::string_view text(arg);
        if (text == kEnableArgument)
            continue;
        if (startsWith(text, kDirectoryPrefix))
            takeValue(options.discoveryDirectory, text, kDirectoryPrefix);
        else if (startsWith(text, kReadyFilePrefix))
            takeValue(options.readyFile, text, kReadyFilePrefix);
        else if (startsWith(text, kLaunchNoncePrefix))
            takeValue(options.launchNonce, text, kLaunchNoncePrefix);
        else if (startsWith(text, kEnableArgument))
            addError(options.error, "unknown control API option (expected --control-api, --control-api-dir=, "
                                    "--control-api-ready-file= or --control-api-launch-nonce=)");
    }

    if (options.discoveryDirectory) {
        const auto reason = controlCheckAbsolutePath(*options.discoveryDirectory, style);
        if (!reason.empty())
            addError(options.error, "--control-api-dir is not an accepted absolute path (" + reason + ")");
    }
    if (options.readyFile) {
        const auto reason = controlCheckAbsolutePath(*options.readyFile, style);
        if (!reason.empty())
            addError(options.error, "--control-api-ready-file is not an accepted absolute path (" + reason + ")");
    }
    if (options.launchNonce && !isValidLaunchNonce(*options.launchNonce))
        addError(options.error, "--control-api-launch-nonce must be 16 to 128 characters of A-Z a-z 0-9 _ -");
    if (options.dataDirectory) {
        const auto reason = controlCheckAbsolutePath(*options.dataDirectory, style);
        if (!reason.empty())
            addError(options.error, "OPENTUNE_DATA_DIR is not an accepted absolute path (" + reason + ")");
    }
    return options;
}

} // namespace OpenTune
