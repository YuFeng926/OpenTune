// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlPathPolicy.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace OpenTune {

/** Reads an environment variable as UTF-8; nullopt when it is not set. */
using ControlEnvironmentLookup = std::function<std::optional<std::string>(const char* name)>;

/** Reads the process environment (GetEnvironmentVariableW on Windows, getenv elsewhere). Thread-safe
    as long as no other thread changes the environment. */
std::optional<std::string> controlSystemEnvironment(const char* name);

/**
 * Run-time switch and options of the control API (PROTOCOL.md sections 6.2 and 6.3).
 *
 * The API is enabled only when the environment variable OPENTUNE_CONTROL_API is exactly "1" or the
 * command line contains the argument --control-api. Only then are --control-api-dir=,
 * --control-api-ready-file= and --control-api-launch-nonce= read and checked; otherwise they are
 * ignored and no error is reported.
 */
struct ControlApiOptions
{
    bool enabled = false;
    /** --control-api-dir=<absolute directory>: the discovery directory. */
    std::optional<std::string> discoveryDirectory;
    /** --control-api-ready-file=<absolute file path>. */
    std::optional<std::string> readyFile;
    /** --control-api-launch-nonce=<16 to 128 of A-Z a-z 0-9 _ ->. */
    std::optional<std::string> launchNonce;
    /** OPENTUNE_DATA_DIR, when set. Checked only when the API is enabled. */
    std::optional<std::string> dataDirectory;
    /**
     * Empty when the API may start; otherwise why it must not start (PROTOCOL.md section 6.4). The
     * text names the option and the rule, never the value, so that it can be logged.
     */
    std::string error;

    bool canStart() const { return enabled && error.empty(); }
};

/**
 * Parses the command-line arguments (UTF-8, without the executable name) and the environment.
 *
 * When the API is enabled, these make error non-empty: a path option whose value breaks
 * PROTOCOL.md section 20.1 for the given style, a launch nonce of the wrong form, an option given
 * more than once, an argument that starts with "--control-api" but is none of the known forms
 * (for example "--control-api-dir" without "="), and OPENTUNE_DATA_DIR set to a path that is not
 * absolute. Pure function; thread-safe.
 */
ControlApiOptions parseControlApiOptions(const std::vector<std::string>& args, const ControlEnvironmentLookup& env,
                                         ControlPathStyle style = controlNativePathStyle());

} // namespace OpenTune
