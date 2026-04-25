// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Small owner/dispatch wrapper for a single GRBL transport.
 */
#ifndef INK_AXIDRAW_GRBL_LINK_H
#define INK_AXIDRAW_GRBL_LINK_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace Inkscape::Axidraw {

class SerialPort;
class TcpPort;

class GrblLink {
public:
    enum class Kind { none, serial, tcp };
    enum class Activity { idle, firmware_sync, streaming };

    GrblLink();
    ~GrblLink();

    GrblLink(GrblLink const &) = delete;
    GrblLink &operator=(GrblLink const &) = delete;
    GrblLink(GrblLink &&) noexcept = delete;
    GrblLink &operator=(GrblLink &&) noexcept = delete;

    void set_serial(std::unique_ptr<SerialPort> port);
    void set_tcp(std::unique_ptr<TcpPort> port);

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] SerialPort *serial_port() const noexcept;
    [[nodiscard]] Activity activity() const noexcept;

    void set_activity(Activity activity) noexcept;

    bool write_bytes(void const *data, std::size_t len);
    bool write_line(std::string const &line);
    bool read_line(std::string &out, int timeout_ms);
    bool send_line_wait_ok(std::string const &line, std::string &err_out);
    void purge_io();
    void close();

private:
    [[nodiscard]] bool query_blocked_bytes(void const *data, std::size_t len) const noexcept;
    [[nodiscard]] bool query_blocked_line(std::string const &line) const noexcept;

    std::unique_ptr<SerialPort> _serial;
    std::unique_ptr<TcpPort> _tcp;
    mutable std::mutex _activity_mutex;
    Activity _activity = Activity::idle;
};

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_GRBL_LINK_H
