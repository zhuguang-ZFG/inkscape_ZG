// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/axidraw/device/tcp-port.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

class LocalTcpServer
{
public:
    LocalTcpServer()
    {
#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif
        _listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!valid(_listen)) {
            throw std::runtime_error("socket failed");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(_listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("bind failed");
        }
        if (::listen(_listen, 1) != 0) {
            throw std::runtime_error("listen failed");
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(_listen, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        _port = ntohs(bound.sin_port);
    }

    ~LocalTcpServer()
    {
        close_socket(_listen);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void serve_fragmented_line(std::string first, std::string second)
    {
        _worker = std::thread([this, first = std::move(first), second = std::move(second)] {
            auto client = ::accept(_listen, nullptr, nullptr);
            if (!valid(client)) {
                return;
            }
            send_all(client, first);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            send_all(client, second);
            close_socket(client);
        });
    }

    void join()
    {
        if (_worker.joinable()) {
            _worker.join();
        }
    }

    int port() const { return _port; }

private:
#ifdef _WIN32
    using socket_t = SOCKET;
    static bool valid(socket_t s) { return s != INVALID_SOCKET; }
#else
    using socket_t = int;
    static bool valid(socket_t s) { return s >= 0; }
#endif

    static void close_socket(socket_t s)
    {
        if (!valid(s)) {
            return;
        }
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
    }

    static void send_all(socket_t s, std::string const &msg)
    {
        char const *p = msg.data();
        std::size_t off = 0;
        while (off < msg.size()) {
#ifdef _WIN32
            int n = ::send(s, p + off, static_cast<int>(msg.size() - off), 0);
#else
            int n = static_cast<int>(::send(s, p + off, msg.size() - off, 0));
#endif
            if (n <= 0) {
                return;
            }
            off += static_cast<std::size_t>(n);
        }
    }

    socket_t _listen{};
    int _port = 0;
    std::thread _worker;
};

} // namespace

TEST(TcpPortReadLineTest, ReadsLineAcrossFragmentedPackets)
{
    LocalTcpServer server;
    server.serve_fragmented_line("HEL", "LO\n");

    Inkscape::Axidraw::TcpPort port;
    ASSERT_TRUE(port.open("127.0.0.1", server.port()));

    std::string line;
    EXPECT_TRUE(port.read_line(line, 1000));
    EXPECT_EQ(line, "HELLO");

    port.close();
    server.join();
}
