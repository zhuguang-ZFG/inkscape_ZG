// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Lightweight GRBL probing over a serial line.
 */
#ifndef INK_AXIDRAW_GRBL_CLIENT_H
#define INK_AXIDRAW_GRBL_CLIENT_H

#include <atomic>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

namespace Inkscape::Axidraw {

class SerialPort;
class TcpPort;

/**
 * While sending long G-code streams, call @a pump between blocking reads so the UI can process
 * events; @a cancel_flag (if not null) aborts the operation when set.
 */
void grbl_begin_plot_waits(std::function<void()> pump, std::atomic<bool> const *cancel_flag);
void grbl_end_plot_waits();

/** Sentinel returned in @a err_out on user cancel. */
char const *grbl_error_user_cancelled() noexcept;

/** Convert low-level GRBL / serial errors into a more user-facing message. */
std::string grbl_error_to_user_message(std::string const &err);
/** True when a controller reply line is an error (case-insensitive). */
bool grbl_is_error_line(std::string_view line);
/** True when a probe response line is specific enough to treat as GRBL. */
bool grbl_is_probe_response_line(std::string_view line);

struct GrblProbeResult {
    bool ok{};
    std::string response_line;
};

/** Probe an already-open serial/TCP GRBL endpoint by sending a status poll (`?`). */
GrblProbeResult probe_open_grbl(SerialPort &port);
GrblProbeResult probe_open_grbl(TcpPort &port);
/** Open @a device, send a status poll (`?`), return first meaningful line. */
GrblProbeResult probe_grbl(std::string const &device, int baud);
/** Connect @a host:@a port over TCP, send `?`, return first meaningful line. */
GrblProbeResult probe_grbl_tcp(std::string const &host, int port);

/** Write a line to GRBL and wait for an `ok` (skips status / echo noise when possible). */
bool grbl_send_line(SerialPort &port, std::string const &line, std::string &err_out);
bool grbl_send_line(TcpPort &port, std::string const &line, std::string &err_out);

/**
 * Send multiple GRBL commands in order, stopping at the first failure.
 * The sender must accept `(std::string const &, std::string &)` and return `bool`.
 */
template <typename Sender>
bool grbl_send_lines(Sender &&send_line_wait_ok, std::initializer_list<std::string_view> lines, std::string &err_out)
{
    for (auto const line : lines) {
        if (!send_line_wait_ok(std::string(line), err_out)) {
            return false;
        }
    }
    return true;
}

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_GRBL_CLIENT_H
