// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Minimal RS-232 access for GRBL controllers (Windows + POSIX).
 */
#ifndef INK_AXIDRAW_SERIAL_PORT_H
#define INK_AXIDRAW_SERIAL_PORT_H

#include <string>
#include <string_view>

namespace Inkscape::Axidraw {

class SerialPort
{
public:
    SerialPort();
    SerialPort(SerialPort const &) = delete;
    SerialPort &operator=(SerialPort const &) = delete;
    ~SerialPort();

    bool open(std::string device, int baud_rate, int open_timeout_ms = 4000);
    void close();
    bool is_open() const;
    bool last_open_timed_out() const;
    bool last_open_access_denied() const;

    void purge_io();

    bool write_bytes(void const *data, size_t len);
    bool write_line(std::string_view line);

    /**
     * Read until the next LF, strip a single trailing CR if present.
     * Incomplete lines stay buffered until a later call completes them.
     * @return false on timeout before assembling a full line.
     */
    bool read_line(std::string &out, int timeout_ms);

private:
    size_t read_chunk(char *buf, size_t cap, int timeout_ms);

    std::string rxbuf_;
#ifdef _WIN32
    void *_handle; // HANDLE
#else
    int _fd;
#endif
    bool _last_open_timed_out = false;
    bool _last_open_access_denied = false;
};

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_SERIAL_PORT_H
