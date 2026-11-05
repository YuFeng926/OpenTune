// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace OpenTune {

/**
 * Secrets and identifiers for the control API, taken only from the operating system's
 * cryptographically secure random number generator: BCryptGenRandom on Windows, arc4random_buf on
 * Apple and BSD systems, getrandom on Linux. General-purpose generators (juce::Random, juce::Uuid,
 * std::rand, std::random_device) are never used, because control tokens must be unpredictable.
 *
 * Threading: all functions are thread-safe.
 */
class SecureRandom
{
public:
    /** Fills the buffer with random bytes. Returns false (and leaves the buffer unspecified) when
        the system generator fails; callers must then refuse to start. */
    static bool fill(void* buffer, std::size_t size);

    /** byteCount random bytes as lowercase hexadecimal (2 * byteCount characters), or nullopt when
        the system generator fails. */
    static std::optional<std::string> hex(std::size_t byteCount);

    /**
     * Compares a candidate (for example a token from session.hello) with the expected secret in
     * time that does not depend on the content of either value: every byte of the expected value is
     * examined, whatever the candidate is. Only the length of the candidate influences timing.
     */
    static bool constantTimeEquals(std::string_view candidate, std::string_view expected);
};

} // namespace OpenTune
