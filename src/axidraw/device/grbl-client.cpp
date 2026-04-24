// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Lightweight GRBL probing over a serial line.
 */

#include "grbl-client.h"

#include "serial-port.h"
#include "tcp-port.h"

#include <chrono>
#include <cctype>
#include <functional>
#include <thread>

#include <glibmm/i18n.h>
#include <glibmm/ustring.h>

namespace Inkscape::Axidraw {

namespace {
char const g_user_cancel_err[] = "INKSCAPE_GRBL_USER_CANCEL";

static std::function<void()> s_pump;
static std::atomic<bool> const *s_cancel = nullptr;

bool contains_ascii_case_insensitive(std::string_view haystack, std::string_view needle)
{
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            auto const h = static_cast<unsigned char>(haystack[i + j]);
            auto const n = static_cast<unsigned char>(needle[j]);
            if (std::tolower(h) != std::tolower(n)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

} // namespace

void grbl_begin_plot_waits(std::function<void()> pump, std::atomic<bool> const *cancel_flag)
{
    s_pump = std::move(pump);
    s_cancel = cancel_flag;
}

void grbl_end_plot_waits()
{
    s_pump = {};
    s_cancel = nullptr;
}

char const *grbl_error_user_cancelled() noexcept
{
    return g_user_cancel_err;
}

std::string grbl_error_to_user_message(std::string const &err)
{
    if (err.empty()) {
        return {};
    }
    if (err == g_user_cancel_err) {
        return _("Operation cancelled.");
    }
    if (err == "serial write failed") {
        return _("Could not write to the serial port. Check the cable, port, baud rate, and controller power.");
    }
    if (err == "timeout waiting for controller response") {
        return _("Timed out waiting for a response from the controller. Check the baud rate, alarms/holds, and "
                 "whether the controller is still powered and connected.");
    }
    constexpr auto k_unexpected = "unexpected response: ";
    if (err.rfind(k_unexpected, 0) == 0) {
        auto const detail = err.substr(std::char_traits<char>::length(k_unexpected));
        return Glib::ustring::compose(_("Unexpected response from the controller:\n%1"), Glib::ustring(detail)).raw();
    }
    if (grbl_is_error_line(err)) {
        return Glib::ustring::compose(_("The controller reported an error:\n%1"), Glib::ustring(err)).raw();
    }
    return err;
}

bool grbl_is_error_line(std::string_view line)
{
    return contains_ascii_case_insensitive(line, "error");
}

static void grbl_progress_tick()
{
    if (s_pump) {
        s_pump();
    }
}

static bool grbl_cancelled(std::string &err_out)
{
    if (s_cancel && s_cancel->load(std::memory_order_relaxed)) {
        err_out = g_user_cancel_err;
        return true;
    }
    return false;
}

static bool grbl_noise_line(std::string const &line)
{
    if (line.empty()) {
        return true;
    }
    char const c = line.front();
    if (c == '<' || c == '[') {
        return true;
    }
    return false;
}

template <typename PortT>
static bool grbl_wait_ok(PortT &port, std::string &err_out)
{
    for (;;) {
        if (grbl_cancelled(err_out)) {
            return false;
        }
        grbl_progress_tick();
        std::string line;
        if (!port.read_line(line, 8000)) {
            err_out = "timeout waiting for controller response";
            return false;
        }
        if (grbl_noise_line(line)) {
            continue;
        }
        if (line.starts_with("ok")) {
            return true;
        }
        if (grbl_is_error_line(line)) {
            err_out = line;
            return false;
        }
        // Likely echoed command text when serial echo is enabled.
        char const c = line.front();
        if (c == 'G' || c == 'M' || c == '$' || c == '?' || c == 'T') {
            continue;
        }
        err_out = "unexpected response: " + line;
        return false;
    }
}

template <typename PortT>
static GrblProbeResult probe_open_grbl_impl(PortT &port)
{
    GrblProbeResult r;
    port.purge_io();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        std::string junk;
        for (int i = 0; i < 20; ++i) {
            if (!port.read_line(junk, 60)) {
                break;
            }
        }
    }

    if (!port.write_line("?")) {
        return r;
    }

    std::string line;
    if (!port.read_line(line, 2500)) {
        return r;
    }

    r.response_line = std::move(line);
    if (grbl_is_error_line(r.response_line)) {
        r.ok = false;
        return r;
    }
    if (r.response_line.find('<') != std::string::npos || r.response_line.find("ok") != std::string::npos) {
        r.ok = true;
        return r;
    }
    r.ok = !r.response_line.empty();
    return r;
}

template <typename PortT>
static bool grbl_send_line_impl(PortT &port, std::string const &line, std::string &err_out)
{
    if (grbl_cancelled(err_out)) {
        return false;
    }
    grbl_progress_tick();
    if (!port.write_line(line)) {
        err_out = "serial write failed";
        return false;
    }
    return grbl_wait_ok(port, err_out);
}

bool grbl_send_line(SerialPort &port, std::string const &line, std::string &err_out)
{
    return grbl_send_line_impl(port, line, err_out);
}

bool grbl_send_line(TcpPort &port, std::string const &line, std::string &err_out)
{
    return grbl_send_line_impl(port, line, err_out);
}

GrblProbeResult probe_open_grbl(SerialPort &port)
{
    return probe_open_grbl_impl(port);
}

GrblProbeResult probe_open_grbl(TcpPort &port)
{
    return probe_open_grbl_impl(port);
}

GrblProbeResult probe_grbl(std::string const &device, int baud)
{
    SerialPort port;
    if (!port.open(device, baud)) {
        return {};
    }
    return probe_open_grbl_impl(port);
}

GrblProbeResult probe_grbl_tcp(std::string const &host, int port)
{
    TcpPort sock;
    if (!sock.open(host, port)) {
        return {};
    }
    return probe_open_grbl_impl(sock);
}

} // namespace Inkscape::Axidraw
