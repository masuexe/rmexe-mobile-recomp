#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
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

namespace rmexe::plat {

#ifdef _WIN32
using sock_t = SOCKET;
inline constexpr sock_t kInvalidSocket = INVALID_SOCKET;
#else
using sock_t = int;
inline constexpr sock_t kInvalidSocket = -1;
#endif

inline void ensure_sockets_init() {
#ifdef _WIN32
    static const bool s_inited = []() {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)s_inited;
#endif
}

inline void close_socket(sock_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

inline bool set_socket_nonblocking(sock_t s, bool nonblocking) {
    if (s == kInvalidSocket) return false;
#ifdef _WIN32
    u_long mode = nonblocking ? 1UL : 0UL;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    int next = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(s, F_SETFL, next) == 0;
#endif
}

inline void set_socket_reuseaddr(sock_t s) {
    int one = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
}

inline void disable_udp_connreset([[maybe_unused]] sock_t s) {
#ifdef _WIN32
    // Prevent Windows UDP socket from returning WSAECONNRESET (10054) on ICMP Port Unreachable
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
               nullptr, 0, &bytes_returned, nullptr, nullptr);
#endif
}

inline bool connect_in_progress() {
#ifdef _WIN32
    int err = ::WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS;
#endif
}

inline bool socket_interrupted() {
#ifdef _WIN32
    return ::WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

inline bool wait_connect_ready(sock_t s, int timeout_ms) {
#ifdef _WIN32
    fd_set wfds;
    fd_set efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(s, &wfds);
    FD_SET(s, &efds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = ::select(0, nullptr, &wfds, &efds, &tv);
    if (rc <= 0 || FD_ISSET(s, &efds) || !FD_ISSET(s, &wfds)) return false;
    int so_err = 0;
    int slen = sizeof(so_err);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &slen) != 0) return false;
    return so_err == 0;
#else
    struct pollfd pfd{s, POLLOUT, 0};
    if (::poll(&pfd, 1, timeout_ms) <= 0) return false;
    int so_err = 0;
    socklen_t slen = sizeof(so_err);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &slen) != 0) return false;
    return so_err == 0;
#endif
}

inline int poll_sockets(pollfd* fds, size_t nfds, int timeout_ms) {
    if (nfds == 0) return 0;
#ifdef _WIN32
    return ::WSAPoll(fds, static_cast<ULONG>(nfds), timeout_ms);
#else
    return ::poll(fds, static_cast<nfds_t>(nfds), timeout_ms);
#endif
}

inline ptrdiff_t sock_send(sock_t s, const void* buf, size_t len) {
#ifdef _WIN32
    return ::send(s, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0);
#else
    #ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
    #else
    constexpr int flags = 0;
    #endif
    return ::send(s, buf, len, flags);
#endif
}

inline ptrdiff_t sock_recv(sock_t s, void* buf, size_t len) {
#ifdef _WIN32
    return ::recv(s, reinterpret_cast<char*>(buf), static_cast<int>(len), 0);
#else
    return ::recv(s, buf, len, 0);
#endif
}

inline ptrdiff_t sock_sendto(sock_t s, const void* buf, size_t len, const sockaddr* dst, socklen_t dst_len) {
#ifdef _WIN32
    return ::sendto(s, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0, dst, static_cast<int>(dst_len));
#else
    return ::sendto(s, buf, len, 0, dst, dst_len);
#endif
}

inline ptrdiff_t sock_recvfrom(sock_t s, void* buf, size_t len, sockaddr* src, socklen_t* src_len) {
#ifdef _WIN32
    int wlen = src_len ? static_cast<int>(*src_len) : 0;
    int rc = ::recvfrom(s, reinterpret_cast<char*>(buf), static_cast<int>(len), 0, src, src_len ? &wlen : nullptr);
    if (src_len && rc >= 0) *src_len = static_cast<socklen_t>(wlen);
    return rc;
#else
    return ::recvfrom(s, buf, len, 0, src, src_len);
#endif
}

} // namespace rmexe::plat
