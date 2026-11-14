#pragma once

#include <juce_events/juce_events.h>

#include <chrono>
#include <functional>

#if JUCE_MAC || JUCE_IOS
#include <CoreFoundation/CoreFoundation.h>
#else
namespace juce {
namespace detail {
bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages);
} // namespace detail
} // namespace juce
#endif

namespace OpenTuneTest {

inline bool dispatchMessagesUntil(const std::function<bool()>& condition, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
#if JUCE_MAC || JUCE_IOS
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
#else
        if (!juce::detail::dispatchNextMessageOnSystemQueue(true))
            juce::Thread::sleep(1);
#endif
    }
    return true;
}

} // namespace OpenTuneTest
