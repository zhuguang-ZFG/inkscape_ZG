// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Small owner/dispatch wrapper for a single GRBL transport.
 */
#ifndef INK_AXIDRAW_GRBL_LINK_H
#define INK_AXIDRAW_GRBL_LINK_H

#include <cstddef>
#include <memory>
#include <string>

namespace Inkscape::Axidraw {

class SerialPort;
class TcpPort;

class GrblLink {
public:
    enum class Kind { none, serial, tcp };

    GrblLink();
    ~GrblLink();

    GrblLink(GrblLink const &) = delete;
    GrblLink &operator=(GrblLink const &) = delete;
    GrblLink(GrblLink &&) noexcept;
    GrblLink &operator=(GrblLink &&) noexcept;

    void set_serial(std::unique_ptr<SerialPort> port);
    void set_tcp(std::unique_ptr<TcpPort> port);

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] SerialPort *serial_port() const noexcept;

    bool write_bytes(void const *data, std::size_t len);
    bool write_line(std::string const &line);
    bool read_line(std::string &out, int timeout_ms);
    bool send_line_wait_ok(std::string const &line, std::string &err_out);
    void purge_io();
    void close();

private:
    std::unique_ptr<SerialPort> _serial;
    std::unique_ptr<TcpPort> _tcp;
};

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_GRBL_LINK_H
