#include "socket.h"

#include <cstring>

#include "mce/log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>

namespace mce::net::detail {
namespace {

std::atomic<u32> g_users{0};

#if defined(_WIN32)
using RawSocket = SOCKET;
RawSocket raw(SocketHandle s) { return static_cast<RawSocket>(s); }
SocketHandle handle(RawSocket s) { return static_cast<SocketHandle>(s); }
bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool in_progress() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
bool set_nonblocking(RawSocket s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
#else
using RawSocket = int;
RawSocket raw(SocketHandle s) { return s; }
SocketHandle handle(RawSocket s) { return s; }
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool in_progress() { return errno == EINPROGRESS || errno == EINTR; }
bool set_nonblocking(RawSocket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

void tune(RawSocket s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
#if defined(__APPLE__)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

// IPv4 address of `host` ("" or "0.0.0.0" = any), false when it does not resolve.
bool resolve(const char* host, u16 port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (host == nullptr || host[0] == '\0') {
        out.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    if (inet_pton(AF_INET, host, &out.sin_addr) == 1) return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &found) != 0 || found == nullptr) return false;
    out.sin_addr = reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr;
    freeaddrinfo(found);
    return true;
}

}  // namespace

SocketHandle invalid_socket() {
#if defined(_WIN32)
    return handle(INVALID_SOCKET);
#else
    return -1;
#endif
}

bool sockets_acquire() {
#if defined(_WIN32)
    if (g_users.fetch_add(1) == 0) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            g_users.fetch_sub(1);
            MCE_LOG_ERROR("net: WSAStartup failed");
            return false;
        }
    }
#else
    g_users.fetch_add(1);
#endif
    return true;
}

void sockets_release() {
    if (g_users.load() == 0) return;
#if defined(_WIN32)
    if (g_users.fetch_sub(1) == 1) WSACleanup();
#else
    g_users.fetch_sub(1);
#endif
}

void close_socket(SocketHandle s) {
    if (s == invalid_socket()) return;
#if defined(_WIN32)
    closesocket(raw(s));
#else
    close(raw(s));
#endif
}

void shutdown_send(SocketHandle s) {
    if (s == invalid_socket()) return;
#if defined(_WIN32)
    shutdown(raw(s), SD_SEND);
#else
    shutdown(raw(s), SHUT_WR);
#endif
}

SocketHandle listen_tcp(const char* address, u16 port, u16& bound_port) {
    sockaddr_in addr;
    if (!resolve(address, port, addr)) {
        MCE_LOG_ERROR("net: cannot resolve bind address '{}'", address);
        return invalid_socket();
    }
    const RawSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle(s) == invalid_socket()) return invalid_socket();
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0 ||
        !set_nonblocking(s)) {
        MCE_LOG_ERROR("net: cannot listen on TCP port {}", port);
        close_socket(handle(s));
        return invalid_socket();
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
    bound_port = ntohs(bound.sin_port);
    return handle(s);
}

SocketHandle accept_tcp(SocketHandle listener) {
    const RawSocket s = accept(raw(listener), nullptr, nullptr);
    if (handle(s) == invalid_socket()) return invalid_socket();
    if (!set_nonblocking(s)) {
        close_socket(handle(s));
        return invalid_socket();
    }
    tune(s);
    return handle(s);
}

SocketHandle connect_tcp(const char* host, u16 port) {
    sockaddr_in addr;
    if (!resolve(host, port, addr)) {
        MCE_LOG_ERROR("net: cannot resolve '{}'", host);
        return invalid_socket();
    }
    const RawSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle(s) == invalid_socket()) return invalid_socket();
    if (!set_nonblocking(s)) {
        close_socket(handle(s));
        return invalid_socket();
    }
    tune(s);
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 && !in_progress()) {
        close_socket(handle(s));
        return invalid_socket();
    }
    return handle(s);
}

ConnectState connect_state(SocketHandle s) {
    const RawSocket r = raw(s);
#if defined(_WIN32)
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(r, &writable);
    FD_SET(r, &failed);
    timeval tv{0, 0};
    const int n = select(0, nullptr, &writable, &failed, &tv);
    if (n < 0 || FD_ISSET(r, &failed)) return ConnectState::Failed;
    if (n == 0) return ConnectState::Pending;
#else
    pollfd p{r, POLLOUT, 0};
    const int n = ::poll(&p, 1, 0);
    if (n < 0) return ConnectState::Failed;
    if (n == 0) return ConnectState::Pending;
#endif
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(r, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) != 0 || error != 0)
        return ConnectState::Failed;
    return ConnectState::Connected;
}

IoResult recv_some(SocketHandle s, u8* data, usize capacity, usize& n) {
    n = 0;
#if defined(_WIN32)
    const int got = recv(raw(s), reinterpret_cast<char*>(data), static_cast<int>(capacity), 0);
#else
    const ssize_t got = recv(raw(s), data, capacity, 0);
#endif
    if (got > 0) {
        n = static_cast<usize>(got);
        return IoResult::Ok;
    }
    if (got == 0) return IoResult::Closed;
    return would_block() ? IoResult::WouldBlock : IoResult::Closed;
}

IoResult send_some(SocketHandle s, const u8* data, usize size, usize& n) {
    n = 0;
    if (size == 0) return IoResult::Ok;
#if defined(_WIN32)
    const int sent = send(raw(s), reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
#elif defined(__linux__)
    const ssize_t sent = send(raw(s), data, size, MSG_NOSIGNAL);
#else
    const ssize_t sent = send(raw(s), data, size, 0);
#endif
    if (sent >= 0) {
        n = static_cast<usize>(sent);
        return IoResult::Ok;
    }
    return would_block() ? IoResult::WouldBlock : IoResult::Closed;
}

}  // namespace mce::net::detail
