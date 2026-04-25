// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Lightweight GRBL probing over a serial line.
 */

#include "grbl-client.h"

#include "serial-port.h"
#include "tcp-port.h"

#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <thread>

#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <glibmm/ustring.h>

namespace Inkscape::Axidraw {

namespace {
char const g_user_cancel_err[] = "INKSCAPE_GRBL_USER_CANCEL";

struct GrblWaitContext {
    std::function<void()> pump;
    std::atomic<bool> const *cancel = nullptr;
};

thread_local GrblWaitContext s_wait_context;
std::mutex g_grbl_debug_log_mutex;

bool starts_with_ascii_case_insensitive(std::string_view haystack, std::string_view needle)
{
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    for (std::size_t j = 0; j < needle.size(); ++j) {
        auto const h = static_cast<unsigned char>(haystack[j]);
        auto const n = static_cast<unsigned char>(needle[j]);
        if (std::tolower(h) != std::tolower(n)) {
            return false;
        }
    }
    return true;
}

} // namespace

void grbl_debug_log_write(char const *source, std::string_view payload)
{
    std::lock_guard const lock(g_grbl_debug_log_mutex);
    auto const path = Glib::build_filename(Glib::get_current_dir(), "grbl-host-write.log");
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) {
        return;
    }

    auto const now = std::chrono::system_clock::now();
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto const tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
    out << timestamp << '.';
    out << std::setw(3) << std::setfill('0') << ms.count();
    out << " [" << (source ? source : "?") << "] ";
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out << '\n';
}

void grbl_begin_plot_waits(std::function<void()> pump, std::atomic<bool> const *cancel_flag)
{
    s_wait_context.pump = std::move(pump);
    s_wait_context.cancel = cancel_flag;
}

void grbl_end_plot_waits()
{
    s_wait_context.pump = {};
    s_wait_context.cancel = nullptr;
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
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    return starts_with_ascii_case_insensitive(line, "error:");
}

bool grbl_is_probe_response_line(std::string_view line)
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    if (line.empty() || grbl_is_error_line(line)) {
        return false;
    }
    if (line.front() == '<') {
        return true;
    }
    if (starts_with_ascii_case_insensitive(line, "ok")) {
        return true;
    }
    if (starts_with_ascii_case_insensitive(line, "grbl")) {
        return true;
    }
    return false;
}

static void grbl_progress_tick()
{
    if (s_wait_context.pump) {
        s_wait_context.pump();
    }
}

static bool grbl_cancelled(std::string &err_out)
{
    if (s_wait_context.cancel && s_wait_context.cancel->load(std::memory_order_relaxed)) {
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
static void grbl_wake_port(PortT &port)
{
    port.purge_io();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    grbl_debug_log_write("grbl_wake_port", "\\r\\n");
    port.write_line("");
    grbl_debug_log_write("grbl_wake_port", "\\r\\n");
    port.write_line("");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    std::string junk;
    for (int i = 0; i < 24; ++i) {
        if (!port.read_line(junk, 80)) {
            break;
        }
    }
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
            grbl_debug_log_write("grbl_read_timeout", "timeout waiting for controller response");
            err_out = "timeout waiting for controller response";
            return false;
        }
        grbl_debug_log_write("grbl_read_line", line);
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
    auto read_probe_lines = [&](int attempts, int timeout_ms) {
        for (int attempt = 0; attempt < attempts; ++attempt) {
            std::string line;
            if (!port.read_line(line, timeout_ms)) {
                continue;
            }
            if (line.empty()) {
                continue;
            }
            r.response_line = std::move(line);
            if (grbl_is_error_line(r.response_line)) {
                r.ok = false;
                return true;
            }
            if (grbl_is_probe_response_line(r.response_line)) {
                r.ok = true;
                return true;
            }
        }
        return false;
    };

    grbl_wake_port(port);

    char const q = '?';
    grbl_debug_log_write("probe_open_grbl", "?");
    if ((port.write_bytes(&q, 1) || port.write_line("?")) && read_probe_lines(8, 700)) {
        return r;
    }

    // Some Bluetooth GRBL variants respond much more reliably to a soft reset
    // banner than to the initial status poll right after opening the SPP link.
    char const ctrl_x = 0x18;
    grbl_debug_log_write("probe_open_grbl", "\\x18");
    if (!port.write_bytes(&ctrl_x, 1)) {
        return r;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (read_probe_lines(10, 800)) {
        return r;
    }

    // One last try after the reset banner / wakeup dance.
    grbl_wake_port(port);
    grbl_debug_log_write("probe_open_grbl", "?");
    if ((port.write_bytes(&q, 1) || port.write_line("?")) && read_probe_lines(8, 800)) {
        return r;
    }

    if (!r.response_line.empty()) {
        return r;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string line;
        if (!port.read_line(line, 1200)) {
            continue;
        }
        if (line.empty()) {
            continue;
        }
        r.response_line = std::move(line);
        break;
    }

    return r;
}

template <typename PortT>
static bool grbl_send_line_impl(PortT &port, std::string const &line, std::string &err_out)
{
    if (grbl_cancelled(err_out)) {
        return false;
    }
    grbl_progress_tick();
    grbl_debug_log_write("grbl_send_line", line);
    if (!port.write_line(line)) {
        err_out = "serial write failed";
        return false;
    }
    if (!grbl_wait_ok(port, err_out)) {
        grbl_debug_log_write("grbl_send_error", line + " => " + err_out);
        return false;
    }
    return true;
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
