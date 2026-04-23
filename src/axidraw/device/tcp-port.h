// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Minimal TCP socket access for GRBL-over-network endpoints.
 */
#ifndef INK_AXIDRAW_TCP_PORT_H
#define INK_AXIDRAW_TCP_PORT_H

#include <string>
#include <string_view>

namespace Inkscape::Axidraw {

class TcpPort
{
public:
    TcpPort();
    TcpPort(TcpPort const &) = delete;
    TcpPort &operator=(TcpPort const &) = delete;
    ~TcpPort();

    bool open(std::string host, int port);
    void close();
    bool is_open() const;

    void purge_io();

    bool write_bytes(void const *data, size_t len);
    bool write_line(std::string_view line);
    bool read_line(std::string &out, int timeout_ms);

private:
    size_t read_chunk(char *buf, size_t cap, int timeout_ms);

    std::string rxbuf_;
#ifdef _WIN32
    void *_socket; // SOCKET
    bool _wsa_started;
#else
    int _fd;
#endif
};

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_TCP_PORT_H
