// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Minimal TCP socket access for GRBL-over-network endpoints.
 */

#include "tcp-port.h"

#include <cstring>
#include <chrono>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Inkscape::Axidraw {

#ifdef _WIN32
static SOCKET as_socket(void *p) { return static_cast<SOCKET>(reinterpret_cast<uintptr_t>(p)); }
static void *to_void_socket(SOCKET s) { return reinterpret_cast<void *>(static_cast<uintptr_t>(s)); }
#endif

TcpPort::TcpPort()
#ifdef _WIN32
    : _socket(nullptr)
    , _wsa_started(false)
#else
    : _fd(-1)
#endif
{
}

TcpPort::~TcpPort()
{
    close();
#ifdef _WIN32
    if (_wsa_started) {
        WSACleanup();
        _wsa_started = false;
    }
#endif
}

bool TcpPort::is_open() const
{
#ifdef _WIN32
    return _socket != nullptr;
#else
    return _fd >= 0;
#endif
}

void TcpPort::close()
{
#ifdef _WIN32
    if (_socket) {
        closesocket(as_socket(_socket));
        _socket = nullptr;
    }
#else
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
#endif
    rxbuf_.clear();
}

bool TcpPort::open(std::string host, int port)
{
    close();
    if (host.empty() || port <= 0 || port > 65535) {
        return false;
    }

#ifdef _WIN32
    if (!_wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        _wsa_started = true;
    }
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string service = std::to_string(port);
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }

    bool ok = false;
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
#ifdef _WIN32
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) {
            continue;
        }
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            _socket = to_void_socket(s);
            ok = true;
            break;
        }
        closesocket(s);
#else
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            _fd = fd;
            ok = true;
            break;
        }
        ::close(fd);
#endif
    }
    freeaddrinfo(res);
    return ok;
}

void TcpPort::purge_io()
{
    rxbuf_.clear();
    if (!is_open()) {
        return;
    }
    char buf[512];
    for (;;) {
        size_t const n = read_chunk(buf, sizeof(buf), 0);
        if (n == 0) {
            break;
        }
    }
}

bool TcpPort::write_bytes(void const *data, size_t len)
{
    if (!is_open() || !data || len == 0) {
        return false;
    }
#ifdef _WIN32
    auto const *p = static_cast<char const *>(data);
    size_t off = 0;
    while (off < len) {
        int n = send(as_socket(_socket), p + off, static_cast<int>(len - off), 0);
        if (n <= 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
#else
    auto const *p = static_cast<char const *>(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(_fd, p + off, len - off, 0);
        if (n <= 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
#endif
}

bool TcpPort::write_line(std::string_view line)
{
    std::string buf(line);
    if (buf.empty() || (buf.back() != '\n' && buf.back() != '\r')) {
        buf.push_back('\n');
    }
    return write_bytes(buf.data(), buf.size());
}

size_t TcpPort::read_chunk(char *buf, size_t cap, int timeout_ms)
{
    if (!is_open() || !buf || cap == 0) {
        return 0;
    }
#ifdef _WIN32
    fd_set rfds;
    FD_ZERO(&rfds);
    SOCKET s = as_socket(_socket);
    FD_SET(s, &rfds);
    timeval tv{};
    timeval *tvp = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }
    int sr = select(0, &rfds, nullptr, nullptr, tvp);
    if (sr <= 0) {
        return 0;
    }
    int n = recv(s, buf, static_cast<int>(cap), 0);
    if (n <= 0) {
        return 0;
    }
    return static_cast<size_t>(n);
#else
    pollfd pfd{};
    pfd.fd = _fd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return 0;
    }
    ssize_t n = ::recv(_fd, buf, cap, 0);
    if (n <= 0) {
        return 0;
    }
    return static_cast<size_t>(n);
#endif
}

bool TcpPort::read_line(std::string &out, int timeout_ms)
{
    out.clear();

    auto pop_line = [&](std::size_t pos) {
        out.assign(rxbuf_.data(), pos);
        if (!out.empty() && out.back() == '\r') {
            out.pop_back();
        }
        rxbuf_.erase(0, pos + 1);
    };

    if (auto p = rxbuf_.find('\n'); p != std::string::npos) {
        pop_line(p);
        return true;
    }

    char tmp[512];
    using clock = std::chrono::steady_clock;
    auto const start = clock::now();
    for (;;) {
        int wait_ms = timeout_ms;
        if (timeout_ms >= 0) {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
            if (elapsed >= timeout_ms) {
                return false;
            }
            wait_ms = timeout_ms - static_cast<int>(elapsed);
        }

        size_t const n = read_chunk(tmp, sizeof(tmp), wait_ms);
        if (n == 0) {
            return false;
        }
        rxbuf_.append(tmp, n);
        if (auto p = rxbuf_.find('\n'); p != std::string::npos) {
            pop_line(p);
            return true;
        }
    }
}

} // namespace Inkscape::Axidraw
