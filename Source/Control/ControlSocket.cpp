// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ControlSocket.h"

#include <climits>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace OpenTune {

namespace {

constexpr const char* kLoopbackAddress = "127.0.0.1";

#if defined(_WIN32)
using NativeSocket = SOCKET;

NativeSocket toNative(std::intptr_t handle)
{
    return static_cast<NativeSocket>(handle);
}

std::intptr_t fromNative(NativeSocket socket)
{
    return socket == INVALID_SOCKET ? -1 : static_cast<std::intptr_t>(socket);
}

int lastSocketError()
{
    return WSAGetLastError();
}

bool lastErrorWouldBlock()
{
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

void closeNative(NativeSocket socket)
{
    closesocket(socket);
}

bool makeNonBlocking(NativeSocket socket)
{
    u_long nonBlocking = 1;
    return ioctlsocket(socket, FIONBIO, &nonBlocking) == 0;
}

NativeSocket createStreamSocket()
{
    // Not inheritable: a child process started by OpenTune must never hold the listener or a session.
    // Overlapped, as socket() would create it: Windows serialises every call on a non-overlapped
    // handle, so a send from a completing thread would wait for the connection thread's poll.
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}
#else
using NativeSocket = int;
constexpr NativeSocket INVALID_SOCKET = -1;

NativeSocket toNative(std::intptr_t handle)
{
    return static_cast<NativeSocket>(handle);
}

std::intptr_t fromNative(NativeSocket socket)
{
    return socket < 0 ? -1 : static_cast<std::intptr_t>(socket);
}

int lastSocketError()
{
    return errno;
}

bool lastErrorWouldBlock()
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

void closeNative(NativeSocket socket)
{
    ::close(socket);
}

bool makeNonBlocking(NativeSocket socket)
{
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

void makeCloseOnExec(NativeSocket socket)
{
    const int flags = fcntl(socket, F_GETFD, 0);
    if (flags >= 0)
        fcntl(socket, F_SETFD, flags | FD_CLOEXEC);
}

NativeSocket createStreamSocket()
{
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket >= 0) {
        makeCloseOnExec(socket);
#if defined(SO_NOSIGPIPE)
        int on = 1;
        setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    }
    return socket;
}
#endif

void configureConnection(NativeSocket socket)
{
#if defined(_WIN32)
    SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
#else
    makeCloseOnExec(socket);
#if defined(SO_NOSIGPIPE)
    int noSigPipe = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
#endif
    makeNonBlocking(socket);
    // Requests and responses are single short lines: send them without Nagle delay.
    int noDelay = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
}

bool loopbackAddress(sockaddr_in& address, int port)
{
    address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    return inet_pton(AF_INET, kLoopbackAddress, &address.sin_addr) == 1;
}

} // namespace

ControlSocket::~ControlSocket()
{
    close();
}

ControlSocket::ControlSocket(ControlSocket&& other) noexcept : handle_(std::exchange(other.handle_, invalidHandle)) {}

ControlSocket& ControlSocket::operator=(ControlSocket&& other) noexcept
{
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, invalidHandle);
    }
    return *this;
}

bool ControlSocket::acquireNetworking()
{
#if defined(_WIN32)
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void ControlSocket::releaseNetworking()
{
#if defined(_WIN32)
    WSACleanup();
#endif
}

ControlSocket ControlSocket::listenOnLoopback(std::string& error)
{
    const NativeSocket socket = createStreamSocket();
    if (socket == INVALID_SOCKET) {
        error = "cannot create the listening socket (error " + std::to_string(lastSocketError()) + ")";
        return {};
    }
    ControlSocket listener(fromNative(socket));
#if defined(_WIN32)
    // No other socket may bind this port while the listener exists.
    BOOL exclusive = TRUE;
    if (setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive))
        != 0) {
        error = "cannot set exclusive address use (error " + std::to_string(lastSocketError()) + ")";
        return {};
    }
#endif
    sockaddr_in address{};
    if (!loopbackAddress(address, 0)) {
        error = "cannot form the loopback address";
        return {};
    }
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = "cannot bind 127.0.0.1 (error " + std::to_string(lastSocketError()) + ")";
        return {};
    }
    if (::listen(socket, SOMAXCONN) != 0) {
        error = "cannot listen (error " + std::to_string(lastSocketError()) + ")";
        return {};
    }
    if (!makeNonBlocking(socket)) {
        error = "cannot make the listener non-blocking (error " + std::to_string(lastSocketError()) + ")";
        return {};
    }
    return listener;
}

ControlSocket ControlSocket::connectToLoopback(int port, int timeoutMs)
{
    const NativeSocket socket = createStreamSocket();
    if (socket == INVALID_SOCKET)
        return {};
    ControlSocket connection(fromNative(socket));
    sockaddr_in address{};
    if (!loopbackAddress(address, port) || !makeNonBlocking(socket))
        return {};
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
#if defined(_WIN32)
        if (WSAGetLastError() != WSAEWOULDBLOCK)
            return {};
        // select() rather than WSAPoll(): WSAPoll does not report failed connection attempts on every
        // Windows version.
        fd_set writable;
        fd_set failedSet;
        FD_ZERO(&writable);
        FD_ZERO(&failedSet);
        FD_SET(socket, &writable);
        FD_SET(socket, &failedSet);
        timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &writable, &failedSet, &timeout) <= 0 || !FD_ISSET(socket, &writable))
            return {};
#else
        if (errno != EINPROGRESS)
            return {};
        pollfd entry{socket, POLLOUT, 0};
        if (::poll(&entry, 1, timeoutMs) <= 0)
            return {};
#endif
        int socketError = 0;
#if defined(_WIN32)
        int length = sizeof(socketError);
#else
        socklen_t length = sizeof(socketError);
#endif
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &length) != 0
            || socketError != 0)
            return {};
    }
    configureConnection(socket);
    return connection;
}

ControlSocket ControlSocket::accept()
{
    if (!isValid())
        return {};
    sockaddr_in peer{};
#if defined(_WIN32)
    int length = sizeof(peer);
#else
    socklen_t length = sizeof(peer);
#endif
    const NativeSocket accepted = ::accept(toNative(handle_), reinterpret_cast<sockaddr*>(&peer), &length);
    if (accepted == INVALID_SOCKET)
        return {};
    configureConnection(accepted);
    return ControlSocket(fromNative(accepted));
}

int ControlSocket::localPort() const
{
    if (!isValid())
        return 0;
    sockaddr_in address{};
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(toNative(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return 0;
    return ntohs(address.sin_port);
}

bool ControlSocket::peerIsLoopback() const
{
    if (!isValid())
        return false;
    sockaddr_in address{};
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getpeername(toNative(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0
        || address.sin_family != AF_INET)
        return false;
    return (ntohl(address.sin_addr.s_addr) >> 24) == 127u;
}

int ControlSocket::poll(bool wantRead, bool wantWrite, int timeoutMs) const
{
    if (!isValid())
        return -1;
#if defined(_WIN32)
    WSAPOLLFD entry{};
    entry.fd = toNative(handle_);
    entry.events = static_cast<SHORT>((wantRead ? POLLRDNORM : 0) | (wantWrite ? POLLWRNORM : 0));
    if (entry.events == 0) {
        Sleep(static_cast<DWORD>(timeoutMs));
        return 0;
    }
    const int count = WSAPoll(&entry, 1, timeoutMs);
    if (count == SOCKET_ERROR)
        return -1;
    const short readFlags = POLLRDNORM | POLLRDBAND;
    const short writeFlags = POLLWRNORM;
#else
    pollfd entry{toNative(handle_), static_cast<short>((wantRead ? POLLIN : 0) | (wantWrite ? POLLOUT : 0)), 0};
    const int count = ::poll(&entry, 1, timeoutMs);
    if (count < 0)
        return errno == EINTR ? 0 : -1;
    const short readFlags = POLLIN;
    const short writeFlags = POLLOUT;
#endif
    if (count == 0)
        return 0;
    int result = 0;
    if ((entry.revents & readFlags) != 0)
        result |= readable;
    if ((entry.revents & writeFlags) != 0)
        result |= writable;
    if ((entry.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
        result |= hangUp | readable;
    return result;
}

long ControlSocket::receive(void* buffer, std::size_t size)
{
    if (!isValid())
        return failed;
    const int request = size > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(size);
    for (;;) {
#if defined(_WIN32)
        const int count = ::recv(toNative(handle_), static_cast<char*>(buffer), request, 0);
#else
        const ssize_t count = ::recv(toNative(handle_), buffer, static_cast<std::size_t>(request), 0);
        if (count < 0 && errno == EINTR)
            continue;
#endif
        if (count >= 0)
            return static_cast<long>(count);
        return lastErrorWouldBlock() ? wouldBlock : failed;
    }
}

long ControlSocket::send(const void* data, std::size_t size)
{
    if (!isValid())
        return failed;
    const int request = size > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(size);
    for (;;) {
#if defined(_WIN32)
        const int count = ::send(toNative(handle_), static_cast<const char*>(data), request, 0);
#else
#if defined(MSG_NOSIGNAL)
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const ssize_t count = ::send(toNative(handle_), data, static_cast<std::size_t>(request), flags);
        if (count < 0 && errno == EINTR)
            continue;
#endif
        if (count >= 0)
            return static_cast<long>(count);
        return lastErrorWouldBlock() ? wouldBlock : failed;
    }
}

void ControlSocket::shutdownSend()
{
    if (!isValid())
        return;
#if defined(_WIN32)
    ::shutdown(toNative(handle_), SD_SEND);
#else
    ::shutdown(toNative(handle_), SHUT_WR);
#endif
}

void ControlSocket::close()
{
    if (!isValid())
        return;
    closeNative(toNative(std::exchange(handle_, invalidHandle)));
}

} // namespace OpenTune
