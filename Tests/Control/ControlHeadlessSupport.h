// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Headless processor test framework: the JUCE message loop without a window, a re-entrant message
// pump, the isolated data directory of the test process and the process environment.
//
// The pump loops over a platform-specific "dispatch one round of messages" step. It does not use
// MessageManager::runDispatchLoop() with callAsync(stopDispatchLoop): in JUCE 8.0.12 stopDispatchLoop()
// latches for the rest of the process (every later posted message is dropped and runDispatchLoop()
// returns at once), so that pattern works only once per process. Both implementations only ever set the
// flag behind hasStopMessageBeenSent() (quitMessagePosted) and never reset it: the generic one in
// juce_events/messages/juce_MessageManager.cpp, used on Windows and Linux (juce_Messaging_linux.cpp defines
// none), and the macOS one in juce_events/native/juce_MessageManager_mac.mm. They differ in when it is set:
// the generic one sets it before the call returns, on any thread; on macOS a call from another thread only
// posts a callback, and the message thread sets it when it dispatches that callback. The difference was
// observed by running these tests on Windows and on macOS.
//
// Windows and Linux: juce::detail::dispatchNextMessageOnSystemQueue(true), which dispatches one message
// and returns false at once when none is pending. JUCE defines it only for those platforms, so it
// cannot be linked on macOS.
// macOS: one CFRunLoopRunInMode() round on the main run loop. JUCE's message queue on macOS is a
// CFRunLoopSource on that run loop, so this dispatches the posted messages without relying on any JUCE
// configuration macro (MessageManager::runDispatchLoopUntil() exists only with
// JUCE_MODAL_LOOPS_PERMITTED, which the product does not enable). It never posts a quit message, so
// the stopDispatchLoop() latch is not engaged.

#include "ControlTestSupport.h"

#include "Control/ControlApiOptions.h"
#include "Control/ControlDiscovery.h"
#include "Control/ControlPathPolicy.h"

#include <juce_events/juce_events.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#if !JUCE_MAC
namespace juce {
namespace detail {
// JUCE's platform message loop (juce_events): dispatches one message of this thread's queue; with
// returnIfNoPendingMessages it returns false at once when none is pending.
bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages);
} // namespace detail
} // namespace juce
#endif

#if !JUCE_WINDOWS
namespace juce {
// JUCE's copy of the process arguments (juce_core); declared the same way as in juce_ApplicationBase.cpp.
extern const char* const* juce_argv;
extern int juce_argc;
} // namespace juce
#endif

namespace OpenTune {
namespace ControlTest {

/**
 * Makes the process arguments visible to juce::JUCEApplicationBase::getCommandLineParameterArray().
 * On Windows JUCE parses GetCommandLineW(), so this does nothing. Elsewhere JUCE reads juce::juce_argv
 * and juce::juce_argc, which only JUCEApplicationBase::main() assigns; a headless test has its own
 * main() and never goes through it, so without this call the control API sees no command line options.
 * Call it first thing in main(), before any ControlService or processor exists.
 */
inline void publishCommandLineToJuce([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#if !JUCE_WINDOWS
    juce::juce_argc = argc;
    juce::juce_argv = argv;
#endif
}

/** Sleep of the message pump when no message is pending. */
constexpr int kPumpIdleMs = 2;

/**
 * The longest time one message took to dispatch in pumpMessagesUntil() since the last reset, in milliseconds.
 * Message thread only. A request that gets no response in time reports it, which tells a message thread kept
 * busy by another message (for example an upstream dialog built from a callback) from a slow method.
 */
inline long long& longestMessageDispatchMs()
{
    static long long longest = 0;
    return longest;
}

/**
 * Dispatches the messages of the calling thread, which must be the message thread, until condition()
 * holds or timeoutMs passes. Returns whether the condition holds. Re-entrant, and usable any number of
 * times per process.
 */
inline bool pumpMessagesUntil(const std::function<bool()>& condition, int timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (condition())
            return true;
        if (Clock::now() >= deadline)
            return false;
        const auto dispatchStart = Clock::now();
#if JUCE_MAC
        // Handles at most one source and returns; waits up to kPumpIdleMs while idle, so no extra
        // sleep is needed.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, kPumpIdleMs / 1000.0, true);
#else
        if (!juce::detail::dispatchNextMessageOnSystemQueue(true))
            sleepMs(kPumpIdleMs);
#endif
        const auto dispatchMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - dispatchStart).count();
        longestMessageDispatchMs() = std::max<long long>(longestMessageDispatchMs(), dispatchMs);
    }
}

/**
 * Dispatches every message posted to the calling (message) thread before this call, then the messages that are
 * pending once those ran (for example the paint of a window one of them opened), until the queue is empty or
 * timeoutMs passes. Returns whether the queue was emptied. Used after background work is over, so that the
 * callbacks it posted run before a scenario's timed requests. JUCE's timers keep posting messages, so the queue
 * is never quiet for long; a marker posted behind the earlier messages tells when those ran.
 */
inline bool drainPostedMessages(int timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    // Shared with the marker, which may still run after a timeout.
    auto reached = std::make_shared<bool>(false);
    if (!juce::MessageManager::callAsync([reached] { *reached = true; }))
        return false;
    if (!pumpMessagesUntil([reached] { return *reached; }, timeoutMs))
        return false;
    while (Clock::now() < deadline) {
#if JUCE_MAC
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true) != kCFRunLoopRunHandledSource)
            return true;
#else
        if (!juce::detail::dispatchNextMessageOnSystemQueue(true))
            return true;
#endif
    }
    return false;
}

/** Dispatches the messages of the calling (message) thread for the given time. */
inline void pumpMessagesFor(int milliseconds)
{
    pumpMessagesUntil([] { return false; }, milliseconds);
}

/** OPENTUNE_DATA_DIR of this process; empty when it is not set to an accepted absolute path. */
inline std::filesystem::path isolatedDataDirectory()
{
    const auto value = controlSystemEnvironment("OPENTUNE_DATA_DIR");
    if (!value || value->empty() || !controlCheckAbsolutePath(*value, controlNativePathStyle()).empty())
        return {};
    return std::filesystem::u8path(*value);
}

/** OPENTUNE_SCENARIO_TIMEOUT_SEC in whole seconds (the ctest TIMEOUT of the test, set by cmake/OpenTuneControl.cmake);
    nullopt when it is not set to a decimal number of at most six digits. */
inline std::optional<long long> scenarioTimeoutSeconds()
{
    const auto value = controlSystemEnvironment("OPENTUNE_SCENARIO_TIMEOUT_SEC");
    if (!value || value->empty() || value->size() > 6 || value->find_first_not_of("0123456789") != std::string::npos)
        return std::nullopt;
    return std::stoll(*value);
}

/** Sets a variable of the process environment, or removes it with nullopt (an empty value also
    removes it on Windows). Visible to controlSystemEnvironment and to child processes. */
inline void setEnvironmentVariable(const char* name, const std::optional<std::string>& value)
{
#if defined(_WIN32)
    // The CRT updates the operating system's environment block as well.
    _putenv_s(name, value ? value->c_str() : "");
#else
    if (value)
        ::setenv(name, value->c_str(), 1);
    else
        ::unsetenv(name);
#endif
}

/** The discovery file this process would write without OPENTUNE_DATA_DIR and --control-api-dir=
    (PROTOCOL.md section 5.1): in the user's real data folder, so no test may ever create it. */
inline std::filesystem::path realDiscoveryFileOfThisProcess()
{
    ControlApiOptions options;
    options.enabled = true;
    std::string error;
    const auto directory =
        controlResolveDiscoveryDirectory(options, controlSystemEnvironment, controlCurrentPlatform(), error);
    if (directory.empty())
        return {};
    return std::filesystem::u8path(directory) / (std::to_string(controlCurrentProcessId()) + ".json");
}

/** True when path names directory or something inside it (lexically; case-insensitive on Windows). */
inline bool isWithin(const std::filesystem::path& path, const std::filesystem::path& directory)
{
    auto normalised = [](const std::filesystem::path& p) {
        auto text = p.lexically_normal().generic_u8string();
#if defined(_WIN32)
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        while (text.size() > 1 && text.back() == '/')
            text.pop_back();
        return text;
    };
    const auto inner = normalised(path);
    const auto outer = normalised(directory);
    return !outer.empty() && inner.compare(0, outer.size(), outer) == 0
           && (inner.size() == outer.size() || inner[outer.size()] == '/');
}

} // namespace ControlTest
} // namespace OpenTune
