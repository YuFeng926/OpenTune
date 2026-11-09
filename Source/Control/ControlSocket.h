// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace OpenTune {

/**
 * Minimal non-blocking TCP socket for the control API, restricted to the IPv4 loopback address.
 * The server uses it for its listener and its connections; tests and local clients use it to
 * connect.
 *
 * Every socket is created non-blocking and is never inherited by child processes. Listeners use
 * exclusive address use on Windows, so no other socket can bind the same port.
 *
 * Threading: a socket belongs to one thread. Only that thread calls its member functions; no other
 * thread closes it, so a handle can never be reused under a concurrent call.
 */
class ControlSocket
{
public:
    /** Bit flags returned by poll(). */
    enum PollResult : int {
        readable = 1,
        writable = 2,
        hangUp = 4
    };
    /** Returned by receive() and send() when the call would block. */
    static constexpr long wouldBlock = -1;
    /** Returned by receive() and send() when the connection failed. */
    static constexpr long failed = -2;

    ControlSocket() = default;
    ~ControlSocket();
    ControlSocket(ControlSocket&& other) noexcept;
    ControlSocket& operator=(ControlSocket&& other) noexcept;
    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    /** Initialises the platform socket library (WSAStartup on Windows). Every successful call is
        balanced by releaseNetworking(). Thread-safe. */
    static bool acquireNetworking();
    static void releaseNetworking();

    /** Creates a listener bound to the literal address 127.0.0.1 on a port chosen by the operating
        system. On failure returns an invalid socket and sets error (no secrets, no paths). */
    static ControlSocket listenOnLoopback(std::string& error);

    /** Connects to 127.0.0.1:port, waiting at most timeoutMs. Invalid socket on failure. */
    static ControlSocket connectToLoopback(int port, int timeoutMs);

    /** Accepts one pending connection of a listener; an invalid socket when none is pending. */
    ControlSocket accept();

    bool isValid() const { return handle_ != invalidHandle; }
    /** The local port, or 0 when unknown. */
    int localPort() const;
    /** True when the peer address is in 127.0.0.0/8. */
    bool peerIsLoopback() const;

    /** Waits at most timeoutMs for the requested conditions. Returns a combination of PollResult
        flags (hangUp also sets readable), 0 on timeout, or -1 on error. */
    int poll(bool wantRead, bool wantWrite, int timeoutMs) const;

    /** Reads at most size bytes: the count (> 0), 0 when the peer closed its side, wouldBlock or
        failed. */
    long receive(void* buffer, std::size_t size);

    /** Writes at most size bytes: the count (>= 0), wouldBlock or failed. */
    long send(const void* data, std::size_t size);

    /** Ends the sending direction (the peer reads end of stream); receiving stays possible. */
    void shutdownSend();

    /** Closes the socket. Safe to call more than once. */
    void close();

private:
    static constexpr std::intptr_t invalidHandle = -1;
    explicit ControlSocket(std::intptr_t handle) : handle_(handle) {}

    std::intptr_t handle_ = invalidHandle;
};

} // namespace OpenTune
