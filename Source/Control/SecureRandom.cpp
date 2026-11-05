// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SecureRandom.h"

#include <climits>
#include <cstdint>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#endif

namespace OpenTune {

bool SecureRandom::fill(void* buffer, std::size_t size)
{
    auto* bytes = static_cast<unsigned char*>(buffer);
#if defined(_WIN32)
    while (size > 0) {
        const auto chunk = static_cast<ULONG>(size > ULONG_MAX ? ULONG_MAX : size);
        // The system-preferred RNG is the kernel CSPRNG; no algorithm handle is needed.
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            return false;
        bytes += chunk;
        size -= chunk;
    }
    return true;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(bytes, size);
    return true;
#elif defined(__linux__)
    while (size > 0) {
        const ssize_t got = getrandom(bytes, size, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        bytes += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
#else
    (void)bytes;
    (void)size;
    return false;
#endif
}

std::optional<std::string> SecureRandom::hex(std::size_t byteCount)
{
    static const char* const digits = "0123456789abcdef";
    std::vector<unsigned char> raw(byteCount);
    if (byteCount > 0 && !fill(raw.data(), raw.size()))
        return std::nullopt;
    std::string out;
    out.reserve(byteCount * 2);
    for (const unsigned char b : raw) {
        out += digits[b >> 4];
        out += digits[b & 0x0F];
    }
    return out;
}

bool SecureRandom::constantTimeEquals(std::string_view candidate, std::string_view expected)
{
    unsigned int difference = candidate.size() == expected.size() ? 0u : 1u;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        // The branch depends only on the candidate's length, never on secret content.
        const auto c = i < candidate.size() ? static_cast<unsigned char>(candidate[i]) : 0u;
        difference |= c ^ static_cast<unsigned char>(expected[i]);
    }
    return difference == 0u;
}

} // namespace OpenTune
