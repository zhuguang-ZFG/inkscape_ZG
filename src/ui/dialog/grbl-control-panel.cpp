// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Serial GRBL / axis jog control (dockable dialog).
 *
 * \par Reference (decompiled Java only)
 * From kxnx `tools/dump_agent/windows_reverse/decompiled_java/com/kvenjoy/drawsoft/lib/remote/pck/PrintPck.java`
 * and `com/kvenjoy/drawsoft/lib/e/f.java` (`a(String s)` is true when `s == null || s.length() == 0`):
 *   - `public String singleGcode;` â€?in `write(b)`, if `f.a(this.uuid)` (uuid empty) is true, the
 *     code writes byte `0` then the UTF-8 bytes of `singleGcode` (length-prefixed via `b2.b` / `b2.a`);
 *   - if `!f.a(this.uuid)` (uuid not empty), it writes byte `1`, then `uuid` and `public int repeat;`
 *     and returns (no `singleGcode` in that branch in the decompiled source).
 * This dialog does not implement that packet format: it uses a local `SerialPort` and `grbl_send_line`
 * in C++ only; line splitting and comments are handled below without claiming parity with the Java host.
 */

#include "grbl-control-panel.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#include <glib.h>
#include <giomm/liststore.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/textbuffer.h>
#include <gtkmm/window.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/ustring.h>

#include <2geom/pathvector.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/serial-port-scan.h"
#include "axidraw/device/serial-port.h"
#include "axidraw/device/tcp-port.h"
#include "axidraw/pipeline/grbl-export.h"
#include "desktop.h"
#include "document.h"
#include "io/sys.h"
#include "preferences.h"
#include "style-enums.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/pack.h"
#include "util/scope_exit.h"

using Inkscape::Axidraw::grbl_begin_plot_waits;
using Inkscape::Axidraw::grbl_end_plot_waits;
using Inkscape::Axidraw::grbl_error_user_cancelled;
using Inkscape::Axidraw::build_grbl_plot_gcode_string;
using Inkscape::Axidraw::build_grbl_plot_machine_preview_pathvector_in_doc_space;
using Inkscape::Axidraw::build_grbl_plot_preview_pathvector;
using Inkscape::Axidraw::GrblPlotStats;
using Inkscape::Axidraw::grbl_export_params_from_preferences;
using Inkscape::Axidraw::grbl_send_line;
using Inkscape::Axidraw::SerialPort;
using Inkscape::CanvasItemBpath;
using Inkscape::choose_file_open;
using Inkscape::choose_file_save;

namespace {

void trim_in_place(std::string &s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    auto it = s.begin();
    while (it != s.end() && (*it == ' ' || *it == '\t')) {
        ++it;
    }
    s.erase(s.begin(), it);
}

/// Skip empty, `;` comments, and parenthesis-only comment lines; keep inline `(â€?` on G-code.
bool should_skip_gcode_line(std::string const &s)
{
    if (s.empty()) {
        return true;
    }
    if (s[0] == ';') {
        return true;
    }
    if (s[0] == '(') {
        auto const end = s.find(')');
        if (end != std::string::npos && end + 1 == s.size()) {
            return true;
        }
    }
    return false;
}

std::string detect_radio_mode_from_reply(std::string reply)
{
    for (auto &c : reply) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    auto has_token = [&](char const *tok) {
        std::string const t(tok);
        auto p = reply.find(t);
        if (p == std::string::npos) {
            return false;
        }
        auto boundary = [&](std::size_t idx) {
            if (idx >= reply.size()) {
                return true;
            }
            char const ch = reply[idx];
            return !(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_');
        };
        bool const left_ok = (p == 0) ? true : boundary(p - 1);
        bool const right_ok = boundary(p + t.size());
        return left_ok && right_ok;
    };
    if (has_token("STA")) return "STA";
    if (has_token("AP")) return "AP";
    if (has_token("BT")) return "BT";
    if (has_token("OFF") || has_token("NONE")) return "OFF";
    return {};
}

constexpr std::size_t k_max_gcode_stream_lines = 200000;

/// Counts lines that `on_send_gcode` would send (same skip rules). Returns @c k_max_gcode_stream_lines + 1 if more
/// than that many executable lines exist.
std::size_t count_executable_gcode_lines(std::string const &text)
{
    std::size_t n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        trim_in_place(line);
        if (should_skip_gcode_line(line)) {
            continue;
        }
        ++n;
        if (n > k_max_gcode_stream_lines) {
            return k_max_gcode_stream_lines + 1;
        }
    }
    return n;
}

constexpr int k_gcode_send_progress_min_interval_ms = 350;
constexpr std::size_t k_gcode_send_progress_line_stride = 80;

constexpr auto k_pref_device = "/options/grbl/serial-device";
constexpr auto k_pref_baud = "/options/grbl/baud";
constexpr auto k_pref_travel = "/options/grbl/feed-travel-mmmin";
constexpr auto k_pref_pen_control = "/options/grbl/pen-control";
constexpr auto k_pref_pen_up = "/options/grbl/pen-up-cmd";
constexpr auto k_pref_pen_down = "/options/grbl/pen-down-cmd";
constexpr auto k_pref_limit_layer = "/options/grbl/limit-to-current-layer";
constexpr auto k_pref_autoprobe_connect = "/options/grbl/auto-probe-connect-on-startup";
constexpr auto k_pref_net_host = "/options/grbl/net-host";
constexpr auto k_pref_net_port = "/options/grbl/net-port";
/// Last folder for G-code save/open dialogs in this panel.
constexpr auto k_pref_save_gcode_dir = "/dialogs/grblcontrol/save_gcode_dir";
constexpr std::size_t k_max_gcode_editor_bytes = 32u * 1024u * 1024u;

int auto_probe_port_priority(std::string const &port)
{
    // Lower is better. Prefer common USB serial adapters first.
    if (port.find("ttyUSB") != std::string::npos || port.find("ttyACM") != std::string::npos ||
        port.find("cu.usb") != std::string::npos || port.find("usbserial") != std::string::npos ||
        port.find("usbmodem") != std::string::npos) {
        return 0;
    }
    if (port.find("ttyAMA") != std::string::npos || port.find("COM") == 0) {
        return 1;
    }
    if (port.find("rfcomm") != std::string::npos || port.find("bluetooth") != std::string::npos ||
        port.find("Bluetooth") != std::string::npos || port.find("BTH") != std::string::npos) {
        return 3;
    }
    return 2;
}

bool parse_tcp_device_spec(std::string const &spec, std::string &host_out, int &port_out)
{
    std::string s = spec;
    if (s.rfind("tcp://", 0) == 0) {
        s.erase(0, 6);
    }
    auto const colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
        return false;
    }
    host_out = s.substr(0, colon);
    try {
        int const p = std::stoi(s.substr(colon + 1));
        if (p <= 0 || p > 65535) {
            return false;
        }
        port_out = p;
        return !host_out.empty();
    } catch (...) {
        return false;
    }
}

Glib::ustring describe_probe_failure_ui(Glib::ustring const &device, int baud,
                                        Inkscape::Axidraw::GrblProbeResult const &probe)
{
    if (probe.response_line.empty()) {
        return Glib::ustring::compose(
            _("No GRBL response on %1 at %2 baud. Check the port, baud rate, and controller power."), device, baud);
    }
    return Glib::ustring::compose(_("Port %1 answered at %2 baud, but not like a GRBL controller:\n%3"), device, baud,
                                  Glib::ustring(probe.response_line));
}

} // namespace

namespace Inkscape::UI::Dialog {

GrblControlPanel::GrblControlPanel()
    : DialogBase("/dialogs/grblcontrol", "GrblControl")
    , _btn_mech_home(_("Mechanical _home ($H)"))
    , _btn_yp(_("_Y+"))
    , _btn_set_origin(_("S_et origin (G92)"))
    , _btn_xm(_("_Xâˆ?))
    , _btn_goto_work_zero(_("G_o to work XY zero"))
    , _btn_xp(_("_X+"))
    , _btn_reset(_("_Reset controller"))
    , _btn_ym(_("_Yâˆ?))
    , _btn_pen_up(_("_Pen up"))
    , _btn_pen_down(_("Pen _down"))
    , _btn_motors(_("M_otor sleep ($SLP)"))
    , _btn_clear_alarm(_("Clear alar_m ($X)"))
    , _btn_load_gcode(_("L_oad G-codeâ€?))
    , _btn_fill_from_drawing(_("Fill from _drawing"))
    , _btn_save_gcode(_("Save G-code _asâ€?))
    , _btn_send_gcode(_("_Send to machine"))
    , _btn_cancel_gcode(_("_Cancel send"))
    , _port_lbl(_("Serial port"))
    , _btn_refresh_ports(_("Refresh ports"))
    , _chk_canvas_plot_preview(_("Document-space _preview"))
    , _chk_machine_space_preview(_("Machine-space _preview (mm â†?canvas)"))
    , _chk_send_from_cursor_line(_("Send from _cursor line downward only"))
    , _btn_read_radio_mode(_("Read mode"))
    , _btn_apply_radio_mode(_("Apply radio mode"))
{
    build_ui();
}

GrblControlPanel::~GrblControlPanel()
{
    ensure_machine_status_poll(false);
    std::lock_guard const lk(_port_mutex);
    link_close();
}

void GrblControlPanel::on_map()
{
    DialogBase::on_map();
    refresh_port_list();
    maybe_auto_probe_and_connect();
    if (_btn_connect.get_active()) {
        ensure_machine_status_poll(true);
    }
    if (_chk_canvas_plot_preview.get_active() || _chk_machine_space_preview.get_active()) {
        sync_plot_preview_overlay();
    }
}

void GrblControlPanel::on_unmap()
{
    clear_plot_preview_overlay();
    ensure_machine_status_poll(false);
    DialogBase::on_unmap();
}

void GrblControlPanel::post_status(Glib::ustring const &text, bool const is_error)
{
    Glib::signal_idle().connect_once(sigc::track_object([this, text, is_error] {
        _status.set_use_markup(is_error);
        if (is_error) {
            _status.set_markup("<span foreground=\"red\">" + Glib::Markup::escape_text(text) + "</span>");
        } else {
            _status.set_text(text);
        }
    }, *this));
}

void GrblControlPanel::post_machine_status(Glib::ustring const &text)
{
    Glib::signal_idle().connect_once(sigc::track_object([this, text] {
        _machine_status.set_text(text);
    }, *this));
}

bool GrblControlPanel::link_is_open() const
{
    return (_port && _port->is_open()) || (_tcp_port && _tcp_port->is_open());
}

bool GrblControlPanel::link_write_bytes(void const *data, size_t len)
{
    if (_port && _port->is_open()) {
        return _port->write_bytes(data, len);
    }
    if (_tcp_port && _tcp_port->is_open()) {
        return _tcp_port->write_bytes(data, len);
    }
    return false;
}

bool GrblControlPanel::link_read_line(std::string &out, int timeout_ms)
{
    if (_port && _port->is_open()) {
        return _port->read_line(out, timeout_ms);
    }
    if (_tcp_port && _tcp_port->is_open()) {
        return _tcp_port->read_line(out, timeout_ms);
    }
    return false;
}

bool GrblControlPanel::link_write_line(std::string const &line, std::string &err_out)
{
    if (_port && _port->is_open()) {
        return grbl_send_line(*_port, line, err_out);
    }
    if (_tcp_port && _tcp_port->is_open()) {
        return grbl_send_line(*_tcp_port, line, err_out);
    }
    err_out = "not connected";
    return false;
}

void GrblControlPanel::link_purge_io()
{
    if (_port && _port->is_open()) {
        _port->purge_io();
    } else if (_tcp_port && _tcp_port->is_open()) {
        _tcp_port->purge_io();
    }
}

void GrblControlPanel::link_close()
{
    if (_port) {
        _port->close();
        _port.reset();
    }
    if (_tcp_port) {
        _tcp_port->close();
        _tcp_port.reset();
    }
}

void GrblControlPanel::ensure_machine_status_poll(bool const on)
{
    _machine_status_poll.disconnect();
    if (!on) {
        return;
    }
    _machine_status_poll = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &GrblControlPanel::on_machine_status_poll_timeout), 1500);
}

bool GrblControlPanel::on_machine_status_poll_timeout()
{
    if (!_btn_connect.get_active()) {
        return false;
    }
    if (_gcode_sending.load(std::memory_order_acquire)) {
        return true;
    }

    std::thread([this] {
        std::unique_lock<std::mutex> lk(_port_mutex, std::try_to_lock);
        if (!lk.owns_lock()) {
            return;
        }
        if (!link_is_open()) {
            return;
        }
        char const q = '?';
        if (!link_write_bytes(&q, 1)) {
            return;
        }
        std::string line;
        if (!link_read_line(line, 400)) {
            return;
        }
        // If a prior command left an "ok" ahead of the report, read once more.
        if (line == "ok" && link_read_line(line, 200)) {
            // use second line
        }
        post_machine_status(Glib::ustring(line));
    }).detach();

    return true;
}

void GrblControlPanel::refresh_port_list()
{
    _suspend_port_combo = true;
    auto *prefs = Inkscape::Preferences::get();
    Glib::ustring const cur_pref = prefs->getString(k_pref_device);
    Glib::ustring const net_host = prefs->getString(k_pref_net_host);
    int const net_port = prefs->getIntLimited(k_pref_net_port, 23, 1, 65535);
    std::vector<std::string> ports = Inkscape::Axidraw::enumerate_serial_ports();
    _port_combo.remove_all();

    bool seen_pref = false;
    for (auto const &p : ports) {
        Glib::ustring const u(p);
        _port_combo.append(u, u);
        if (u == cur_pref) {
            seen_pref = true;
        }
    }
    if (!cur_pref.empty() && !seen_pref) {
        _port_combo.append(cur_pref, cur_pref);
    }
    if (!net_host.empty()) {
        Glib::ustring const tcp_spec = "tcp://" + net_host + ":" + std::to_string(net_port);
        if (tcp_spec != cur_pref) {
            _port_combo.append(tcp_spec, tcp_spec);
        }
    }

    if (!cur_pref.empty()) {
        _port_combo.set_active_id(cur_pref);
    } else if (ports.empty()) {
        // leave empty
    } else {
        _port_combo.set_active(0);
    }
    _suspend_port_combo = false;
}

void GrblControlPanel::maybe_auto_probe_and_connect()
{
    if (_auto_probe_attempted) {
        return;
    }
    _auto_probe_attempted = true;

    auto *prefs = Inkscape::Preferences::get();
    if (!prefs->getBool(k_pref_autoprobe_connect, true)) {
        return;
    }
    if (_btn_connect.get_active() || _connecting.load(std::memory_order_acquire)) {
        return;
    }

    Glib::ustring const pref_dev = prefs->getString(k_pref_device);
    std::string pref_host;
    int pref_port = 0;
    if ((pref_dev.empty() && !prefs->getString(k_pref_net_host).empty())) {
        pref_host = prefs->getString(k_pref_net_host).raw();
        pref_port = prefs->getIntLimited(k_pref_net_port, 23, 1, 65535);
    } else {
        parse_tcp_device_spec(pref_dev.raw(), pref_host, pref_port);
    }
    if (!pref_host.empty() && pref_port > 0) {
        std::string const pref_spec = pref_dev.empty() ? ("tcp://" + pref_host + ":" + std::to_string(pref_port))
                                                       : pref_dev.raw();
        post_status(Glib::ustring::compose(_("Auto probe: checking %1 over TCPâ€?), pref_spec), false);
        std::thread([this, pref_dev = pref_spec, pref_host, pref_port] {
            auto const probe = Inkscape::Axidraw::probe_grbl_tcp(pref_host, pref_port);
            Glib::signal_idle().connect_once(sigc::track_object([this, pref = Glib::ustring(pref_dev), probe] {
                if (!probe.ok) {
                    post_status(_("Auto probe could not reach GRBL over the configured TCP endpoint."), false);
                    return;
                }
                if (_btn_connect.get_active() || _connecting.load(std::memory_order_acquire)) {
                    return;
                }
                _suspend_port_combo = true;
                _port_combo.set_active_id(pref);
                if (_port_combo.get_active_id().empty()) {
                    _port_combo.append(pref, pref);
                    _port_combo.set_active_id(pref);
                }
                _suspend_port_combo = false;
                post_status(Glib::ustring::compose(_("Auto probe matched %1; connectingâ€?), pref), false);
                _btn_connect.set_active(true);
            }, *this));
        }).detach();
        return;
    }

    std::vector<std::string> ports = Inkscape::Axidraw::enumerate_serial_ports();
    std::stable_sort(ports.begin(), ports.end(), [](std::string const &a, std::string const &b) {
        int const pa = auto_probe_port_priority(a);
        int const pb = auto_probe_port_priority(b);
        if (pa != pb) {
            return pa < pb;
        }
        return a < b;
    });
    std::vector<std::string> candidates;
    candidates.reserve(ports.size() + 1);
    if (!pref_dev.empty()) {
        candidates.push_back(pref_dev.raw());
    }
    for (auto const &p : ports) {
        if (pref_dev.empty() || p != pref_dev.raw()) {
            candidates.push_back(p);
        }
    }
    if (candidates.empty()) {
        return;
    }

    int const baud = prefs->getIntLimited(k_pref_baud, 115200, 9600, 230400);
    post_status(_("Auto probe: scanning serial ports for a GRBL controllerâ€?), false);
    std::thread([this, candidates = std::move(candidates), baud]() mutable {
        bool found = false;
        std::string chosen;
        for (auto const &dev : candidates) {
            auto const probe = Inkscape::Axidraw::probe_grbl(dev, baud);
            if (probe.ok) {
                found = true;
                chosen = dev;
                break;
            }
        }

        Glib::signal_idle().connect_once(sigc::track_object([this, found, chosen = Glib::ustring(chosen), baud] {
            if (!found) {
                post_status(_("Auto probe found no reachable GRBL controller. You can choose a port and connect manually."),
                            false);
                return;
            }
            if (_btn_connect.get_active() || _connecting.load(std::memory_order_acquire)) {
                return;
            }

            auto *prefs = Inkscape::Preferences::get();
            _suspend_port_combo = true;
            _port_combo.set_active_id(chosen);
            if (_port_combo.get_active_id().empty()) {
                _port_combo.append(chosen, chosen);
                _port_combo.set_active_id(chosen);
            }
            _suspend_port_combo = false;
            prefs->setString(k_pref_device, chosen);
            prefs->save();
            post_status(Glib::ustring::compose(_("Auto probe matched %1 (%2 baud); connectingâ€?), chosen, baud),
                        false);
            _btn_connect.set_active(true);
        }, *this));
    }).detach();
}

void GrblControlPanel::on_port_combo_changed()
{
    if (_suspend_port_combo) {
        return;
    }
    Glib::ustring id = _port_combo.get_active_id();
    if (id.empty()) {
        id = _port_combo.get_active_text();
    }
    if (id.empty()) {
        return;
    }
    auto *prefs = Inkscape::Preferences::get();
    prefs->setString(k_pref_device, id);
    prefs->save();
}

void GrblControlPanel::run_action(std::function<void(std::string &)> work, bool const report_ok)
{
    std::thread([this, w = std::move(work), report_ok]() mutable {
        std::lock_guard const guard(_port_mutex);
        if (!link_is_open()) {
            post_status(_("Not connected."), true);
            return;
        }
        std::string err;
        w(err);
        if (err.empty()) {
            if (report_ok) {
                post_status(_("OK"), false);
            }
        } else {
            post_status(Glib::ustring(Inkscape::Axidraw::grbl_error_to_user_message(err)), true);
        }
    }).detach();
}

void GrblControlPanel::connect_toggle()
{
    bool const want = _btn_connect.get_active();
    if (!want) {
        _connecting.store(false, std::memory_order_release);
        ensure_machine_status_poll(false);
        update_connection_controls();
        std::lock_guard const lk(_port_mutex);
        if (link_is_open()) {
            link_close();
            post_status(_("Controller link closed."), false);
        }
        post_machine_status({});
        return;
    }

    auto *prefs = Inkscape::Preferences::get();
    Glib::ustring device = prefs->getString(k_pref_device);
    if (device.empty()) {
        device = _port_combo.get_active_id();
        if (device.empty()) {
            device = _port_combo.get_active_text();
        }
        if (!device.empty()) {
            prefs->setString(k_pref_device, device);
            prefs->save();
        }
    }
    if (device.empty()) {
        _btn_connect.set_active(false);
        post_status(_("Choose a serial port above or set â€œSerial deviceâ€?in Preferences (GRBL tab)."), true);
        return;
    }

    int const baud = prefs->getIntLimited(k_pref_baud, 115200, 9600, 230400);
    Glib::ustring device_for_thread = device;
    std::string tcp_host;
    int tcp_port = 0;
    bool const tcp_mode = parse_tcp_device_spec(device.raw(), tcp_host, tcp_port);
    if (!tcp_mode && device_for_thread.empty()) {
        Glib::ustring const host = prefs->getString(k_pref_net_host);
        int const net_port = prefs->getIntLimited(k_pref_net_port, 23, 1, 65535);
        if (!host.empty()) {
            device_for_thread = "tcp://" + host + ":" + std::to_string(net_port);
            tcp_host = host.raw();
            tcp_port = net_port;
        }
    }
    bool const use_tcp = !tcp_host.empty() && tcp_port > 0;
    _connecting.store(true, std::memory_order_release);
    update_connection_controls();
    if (use_tcp) {
        post_status(Glib::ustring::compose(_("Probing %1 over TCPâ€?), device_for_thread), false);
    } else {
        post_status(Glib::ustring::compose(_("Probing %1 at %2 baudâ€?), device_for_thread, baud), false);
    }

    // Run open on a worker to avoid blocking UI if driver stalls.
    std::thread([this, device_for_thread, baud, use_tcp, tcp_host, tcp_port] {
        auto const probe =
            use_tcp ? Inkscape::Axidraw::probe_grbl_tcp(tcp_host, tcp_port)
                    : Inkscape::Axidraw::probe_grbl(device_for_thread.raw(), baud);
        if (!probe.ok) {
            Glib::signal_idle().connect_once(sigc::track_object([this, probe, baud, dev = Glib::ustring(device_for_thread)] {
                _connecting.store(false, std::memory_order_release);
                update_connection_controls();
                if (!_btn_connect.get_active()) {
                    return;
                }
                _btn_connect.set_active(false);
                if (dev.rfind("tcp://", 0) == 0) {
                    Glib::ustring const detail = probe.response_line.empty()
                                                     ? _("No GRBL response over TCP.")
                                                     : Glib::ustring::compose(
                                                           _("TCP endpoint answered, but not like GRBL:\n%1"),
                                                           Glib::ustring(probe.response_line));
                    post_status(Glib::ustring::compose(_("Could not connect to %1.\n%2"), dev, detail), true);
                } else {
                    post_status(describe_probe_failure_ui(dev, baud, probe), true);
                }
                post_machine_status({});
            }, *this));
            return;
        }

        auto port = std::make_unique<SerialPort>();
        auto tcp = std::make_unique<Inkscape::Axidraw::TcpPort>();
        bool const opened = use_tcp ? tcp->open(tcp_host, tcp_port) : port->open(device_for_thread.raw(), baud);
        if (!opened) {
            Glib::signal_idle().connect_once(sigc::track_object([this] {
                _connecting.store(false, std::memory_order_release);
                update_connection_controls();
                if (!_btn_connect.get_active()) {
                    return;
                }
                _btn_connect.set_active(false);
                post_status(_("Could not open the selected link."), true);
            }, *this));
            return;
        }
        if (use_tcp) {
            tcp->purge_io();
        } else {
            port->purge_io();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        {
            std::string junk;
            for (int n = 0; n < 20; n++) {
                bool const got = use_tcp ? tcp->read_line(junk, 25) : port->read_line(junk, 25);
                if (!got) {
                    break;
                }
            }
        }

        std::lock_guard guard(_port_mutex);
        link_close();
        if (use_tcp) {
            _tcp_port = std::move(tcp);
        } else {
            _port = std::move(port);
        }
        Glib::signal_idle().connect_once(sigc::track_object([this, dev = Glib::ustring(device_for_thread), probe] {
            _connecting.store(false, std::memory_order_release);
            update_connection_controls();
            if (!_btn_connect.get_active()) {
                std::lock_guard const lk(_port_mutex);
                link_close();
                return;
            }
            update_connection_controls();
            if (probe.response_line.empty()) {
                post_status(Glib::ustring::compose(_("Connected to %1"), dev), false);
            } else {
                post_status(Glib::ustring::compose(_("Connected to %1\nController: %2"), dev,
                                                   Glib::ustring(probe.response_line)),
                            false);
                post_machine_status(Glib::ustring(probe.response_line));
            }
            ensure_machine_status_poll(true);
        }, *this));
    }).detach();
}

double GrblControlPanel::jog_distance_mm() const
{
    auto const t = _jog_dist.get_active_text();
    return std::strtod(t.c_str(), nullptr);
}

double GrblControlPanel::travel_feed_mm_min() const
{
    auto *prefs = Inkscape::Preferences::get();
    return prefs->getDoubleLimited(k_pref_travel, 6000.0, 60.0, 20000.0);
}

void GrblControlPanel::jog_axis(char const axis, double const sign, double const dist_mm, double const feed)
{
    run_action(
        [this, axis, sign, dist_mm, feed](std::string &e) {
            if (dist_mm <= 0) {
                e = _("Jog distance must be positive");
                return;
            }
            double const d0 = dist_mm * sign;
            std::ostringstream dstr;
            dstr.setf(std::ios::fixed);
            dstr << std::setprecision(6) << d0;
            int const ifeed = static_cast<int>(feed + 0.5);
            if (!link_write_line("G21", e)) {
                return;
            }
            if (!link_write_line("G91", e)) {
                return;
            }
            {
                std::ostringstream m;
                m << "G1 " << axis << dstr.str() << " F" << ifeed;
                if (!link_write_line(m.str(), e)) {
                    return;
                }
            }
            if (!link_write_line("G90", e)) {
                return;
            }
        },
        false);
}

void GrblControlPanel::jog_x(double const sign)
{
    double const d = jog_distance_mm();
    double const f = travel_feed_mm_min();
    jog_axis('X', sign, d, f);
}

void GrblControlPanel::jog_y(double const sign)
{
    double const d = jog_distance_mm();
    double const f = travel_feed_mm_min();
    jog_axis('Y', sign, d, f);
}

void GrblControlPanel::soft_reset()
{
    run_action(
        [this](std::string &e) {
            const char c = 0x18;
            if (!link_write_bytes(&c, 1)) {
                e = _("Could not write soft reset byte to serial");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            link_purge_io();
            post_status(
                _("Soft reset sent. If the port no longer answers, turn â€œConnectâ€?off and on again."), false);
        },
        false);
}

void GrblControlPanel::send_pen_state(bool const up)
{
    run_action([this, up](std::string &e) {
        (void)this;
        auto *prefs = Inkscape::Preferences::get();
        Glib::ustring const pctl = prefs->getString(k_pref_pen_control, "z");
        if (pctl == "m3m5" || pctl == "M3M5") {
            if (!link_write_line(up ? "M5" : "M3 S1000", e)) {
                return;
            }
        } else {
            Glib::ustring const cmd = up ? prefs->getString(k_pref_pen_up, "G1 Z0 F3000") : prefs->getString(k_pref_pen_down, "G1 Z5 F3000");
            if (cmd.empty()) {
                e = up ? _("Pen up command (Preferences) is empty") : _("Pen down command (Preferences) is empty");
                return;
            }
            if (!link_write_line(cmd.raw(), e)) {
                return;
            }
        }
    });
}

void GrblControlPanel::update_connection_controls()
{
    bool const connecting = _connecting.load(std::memory_order_acquire);
    bool const sending = _gcode_sending.load(std::memory_order_acquire);
    bool const serial_controls = !connecting && !sending;

    _port_combo.set_sensitive(serial_controls);
    _btn_refresh_ports.set_sensitive(serial_controls);

    if (connecting) {
        _btn_connect.set_label(_("Connectingâ€?));
    } else if (_btn_connect.get_active()) {
        _btn_connect.set_label(_("Disconnect"));
    } else {
        _btn_connect.set_label(_("Connect"));
    }
}

void GrblControlPanel::set_controls_sensitive_for_gcode_stream(bool const allow_interaction)
{
    _btn_connect.set_sensitive(allow_interaction);
    _btn_load_gcode.set_sensitive(allow_interaction);
    _btn_fill_from_drawing.set_sensitive(allow_interaction);
    _btn_save_gcode.set_sensitive(allow_interaction);
    _chk_send_from_cursor_line.set_sensitive(allow_interaction);
    _chk_canvas_plot_preview.set_sensitive(allow_interaction);
    _chk_machine_space_preview.set_sensitive(allow_interaction);
    _gcode_view.set_sensitive(allow_interaction);
    _btn_send_gcode.set_sensitive(allow_interaction);
    _btn_cancel_gcode.set_sensitive(!allow_interaction);
    for (auto *b : {&_btn_mech_home, &_btn_yp, &_btn_set_origin, &_btn_xm, &_btn_goto_work_zero, &_btn_xp, &_btn_reset, &_btn_ym,
                    &_btn_pen_up, &_btn_pen_down, &_btn_motors, &_btn_clear_alarm}) {
        b->set_sensitive(allow_interaction);
    }
    _jog_dist.set_sensitive(allow_interaction);
    update_connection_controls();
}

void GrblControlPanel::finish_gcode_stream_ui()
{
    Glib::signal_idle().connect_once(sigc::track_object([this] {
        _gcode_sending = false;
        _gcode_cancel = false;
        set_controls_sensitive_for_gcode_stream(true);
    }, *this));
}

void GrblControlPanel::on_cancel_gcode_stream()
{
    _gcode_cancel = true;
}

void GrblControlPanel::clear_plot_preview_overlay()
{
    _plot_preview_overlay.reset();
    _plot_preview_machine_overlay.reset();
}

void GrblControlPanel::sync_plot_preview_overlay()
{
    clear_plot_preview_overlay();
    if (!_chk_canvas_plot_preview.get_active() && !_chk_machine_space_preview.get_active()) {
        return;
    }
    auto *desk = getDesktop();
    auto *doc = getDocument();
    if (!desk || !doc) {
        return;
    }
    auto *prefs = Inkscape::Preferences::get();
    Inkscape::Axidraw::GrblExportParams params;
    grbl_export_params_from_preferences(prefs, params);
    Inkscape::Axidraw::GrblExportContext ctx;
    ctx.desktop = desk;
    ctx.selection = getSelection();
    ctx.use_current_layer_without_selection = prefs->getBool(k_pref_limit_layer, false);
    ctx.cancel = nullptr;

    constexpr std::size_t k_preview_max_strokes = 12000;
    Geom::Affine const aff = desk->doc2dt();
    Glib::ustring status_note;

    if (_chk_canvas_plot_preview.get_active()) {
        Geom::PathVector pv_doc;
        std::string err;
        std::size_t included = 0;
        std::size_t total = 0;
        if (!build_grbl_plot_preview_pathvector(doc, params, ctx, pv_doc, err, k_preview_max_strokes, &included,
                                                &total)) {
            post_status(err.empty() ? Glib::ustring(_("Could not build plot preview.")) : Glib::ustring(err), true);
            if (!_chk_machine_space_preview.get_active()) {
                return;
            }
        } else if (!pv_doc.empty()) {
            Geom::PathVector pv_dt;
            for (auto const &pth : pv_doc) {
                pv_dt.push_back(pth * aff);
            }

            _plot_preview_overlay = make_canvasitem<CanvasItemBpath>(desk->getCanvasTemp(), pv_dt, true);
            _plot_preview_overlay->set_stroke(0x22aaffcc);
            _plot_preview_overlay->set_fill(0x00000000, SP_WIND_RULE_NONZERO);
            _plot_preview_overlay->set_stroke_width(1.0);
            _plot_preview_overlay->set_visible(true);

            if (total > included) {
                status_note = Glib::ustring::compose(
                    _("Document-space preview shows %1 of %2 segments (simplify the drawing or turn that preview off "
                      "to reduce load)."),
                    static_cast<guint64>(included), static_cast<guint64>(total));
            }
        }
    }

    if (!_chk_machine_space_preview.get_active()) {
        return;
    }

    Geom::PathVector pv_m;
    std::string err_m;
    bool clip_approx = false;
    std::size_t inc_m = 0;
    std::size_t tot_m = 0;
    if (!build_grbl_plot_machine_preview_pathvector_in_doc_space(doc, params, ctx, pv_m, err_m, &clip_approx,
                                                                 k_preview_max_strokes, &inc_m, &tot_m)) {
        post_status(err_m.empty() ? Glib::ustring(_("Could not build machine-space plot preview.")) : Glib::ustring(err_m),
                    true);
        return;
    }
    if (!pv_m.empty()) {
        Geom::PathVector pv_m_dt;
        for (auto const &pth : pv_m) {
            pv_m_dt.push_back(pth * aff);
        }
        _plot_preview_machine_overlay = make_canvasitem<CanvasItemBpath>(desk->getCanvasTemp(), pv_m_dt, true);
        _plot_preview_machine_overlay->set_stroke(0xff8844cc);
        _plot_preview_machine_overlay->set_fill(0x00000000, SP_WIND_RULE_NONZERO);
        _plot_preview_machine_overlay->set_stroke_width(1.25);
        _plot_preview_machine_overlay->set_visible(true);
        if (clip_approx && tot_m > inc_m) {
            status_note = Glib::ustring::compose(
                _("Machine-space preview shows %1 of %2 segments and is approximate where bed clipping removed "
                  "geometry."),
                static_cast<guint64>(inc_m), static_cast<guint64>(tot_m));
        } else if (clip_approx) {
            status_note = _("Machine-space preview is approximate where machine-bed clipping removed geometry.");
        } else if (tot_m > inc_m) {
            status_note = Glib::ustring::compose(
                _("Machine-space preview shows %1 of %2 segments (simplify the drawing to preview everything)."),
                static_cast<guint64>(inc_m), static_cast<guint64>(tot_m));
        }
    }
    if (!status_note.empty()) {
        post_status(status_note, false);
    }
}

void GrblControlPanel::on_fill_gcode_from_document()
{
    if (_gcode_sending.load()) {
        return;
    }
    auto *doc = getDocument();
    auto *desktop = getDesktop();
    if (!doc || !desktop) {
        post_status(_("No active document or desktop."), true);
        clear_plot_preview_overlay();
        return;
    }

    auto *prefs = Inkscape::Preferences::get();
    Inkscape::Axidraw::GrblExportParams params;
    grbl_export_params_from_preferences(prefs, params);

    Inkscape::Axidraw::GrblExportContext ctx;
    ctx.desktop = desktop;
    ctx.selection = getSelection();
    ctx.use_current_layer_without_selection = prefs->getBool(k_pref_limit_layer, false);
    ctx.cancel = nullptr;

    std::string out;
    std::string err;
    std::size_t strokes = 0;
    GrblPlotStats stats{};
    if (!build_grbl_plot_gcode_string(doc, params, ctx, out, err, &strokes, k_max_gcode_editor_bytes, &stats)) {
        clear_plot_preview_overlay();
        post_status(err.empty() ? Glib::ustring(_("Could not build G-code from the current document.")) : Glib::ustring(err),
                    true);
        return;
    }
    if (auto const buf = _gcode_view.get_buffer()) {
        buf->set_text(out);
    }
    if (stats.has_bounds_mm) {
        std::ostringstream wxh;
        wxh << std::fixed << std::setprecision(1) << (stats.max_x_mm - stats.min_x_mm) << " Ã— "
            << (stats.max_y_mm - stats.min_y_mm);
        if (stats.has_length_stats && (stats.draw_length_mm + stats.travel_length_mm) > 1e-9) {
            double const total = stats.draw_length_mm + stats.travel_length_mm;
            double const air = (stats.travel_length_mm / total) * 100.0;
            std::ostringstream lengths;
            lengths << std::fixed << std::setprecision(1) << stats.draw_length_mm << " / " << stats.travel_length_mm;
            std::ostringstream ratio;
            ratio << std::fixed << std::setprecision(1) << air;
            post_status(
                Glib::ustring::compose(
                    _("Editor filled: %1 stroke(s), work area about %2 mm, draw/travel %3 mm, air-run %4%%. "
                      "Review, use â€œSave G-code asâ€¦â€?if needed, then â€œSend to machineâ€?"),
                    static_cast<guint64>(strokes), Glib::ustring(wxh.str()), Glib::ustring(lengths.str()),
                    Glib::ustring(ratio.str())),
                false);
        } else {
            post_status(
                Glib::ustring::compose(
                    _("Editor filled: %1 stroke(s), work area about %2 mm (machine coordinates after preferences). "
                      "Review, use â€œSave G-code asâ€¦â€?if needed, then â€œSend to machineâ€?"),
                    static_cast<guint64>(strokes), Glib::ustring(wxh.str())),
                false);
        }
    } else {
        post_status(
            Glib::ustring::compose(
                _("Editor filled with G-code for %1 stroke(s). Review the text, then use â€œSend to machineâ€?when ready."),
                static_cast<guint64>(strokes)),
            false);
    }
    if (_chk_canvas_plot_preview.get_active() || _chk_machine_space_preview.get_active()) {
        sync_plot_preview_overlay();
    }
}

void GrblControlPanel::on_load_gcode_from_file()
{
    if (_gcode_sending.load()) {
        return;
    }
    auto *win = dynamic_cast<Gtk::Window *>(get_root());
    if (!win) {
        post_status(_("Could not open file dialog (no parent window)."), true);
        return;
    }

    std::string folder;
    Inkscape::UI::Dialog::get_start_directory(folder, k_pref_save_gcode_dir, true);

    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto gcf = Gtk::FileFilter::create();
    gcf->set_name(_("G-code"));
    gcf->add_suffix("nc");
    gcf->add_suffix("gcode");
    gcf->add_suffix("tap");
    gcf->add_suffix("cnc");
    gcf->add_suffix("txt");
    filters->append(gcf);
    auto all = Gtk::FileFilter::create();
    all->set_name(_("All files"));
    all->add_pattern("*");
    filters->append(all);

    Glib::RefPtr<Gio::File> const src =
        choose_file_open(_("Load G-code"), win, filters, folder, _("Open"));
    if (!src) {
        return;
    }
    std::string const path = src->get_path();
    if (path.empty()) {
        post_status(_("Could not read file (no local path)."), true);
        return;
    }
    std::string contents;
    try {
        contents = Glib::file_get_contents(path);
    } catch (Glib::FileError const &e) {
        post_status(e.what(), true);
        return;
    }
    if (contents.size() > k_max_gcode_editor_bytes) {
        post_status(_("File is too large to load into the editor."), true);
        return;
    }
    if (auto const buf = _gcode_view.get_buffer()) {
        buf->set_text(contents);
    }
    if (auto *prefs = Inkscape::Preferences::get()) {
        prefs->setString(k_pref_save_gcode_dir, folder);
    }
    post_status(Glib::ustring::compose(_("Loaded G-code from â€?1â€?"), src->get_parse_name()), false);
}

void GrblControlPanel::on_save_gcode_as()
{
    if (_gcode_sending.load()) {
        return;
    }
    Glib::RefPtr<Gtk::TextBuffer> const buf = _gcode_view.get_buffer();
    if (!buf) {
        return;
    }
    Glib::ustring const utext = buf->get_text();
    std::string text = utext.raw();
    if (!std::any_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); })) {
        post_status(_("Nothing to save (G-code is empty)."), true);
        return;
    }
    auto *win = dynamic_cast<Gtk::Window *>(get_root());
    if (!win) {
        post_status(_("Could not open save dialog (no parent window)."), true);
        return;
    }

    std::string folder;
    Inkscape::UI::Dialog::get_start_directory(folder, k_pref_save_gcode_dir, true);

    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto gcf = Gtk::FileFilter::create();
    gcf->set_name(_("G-code"));
    gcf->add_suffix("nc");
    gcf->add_suffix("gcode");
    gcf->add_suffix("tap");
    gcf->add_suffix("cnc");
    gcf->add_suffix("txt");
    filters->append(gcf);
    auto all = Gtk::FileFilter::create();
    all->set_name(_("All files"));
    all->add_pattern("*");
    filters->append(all);

    std::string initial = "plot.nc";
    if (auto *d = getDocument()) {
        if (char const *fn = d->getDocumentFilename()) {
            std::string base = Glib::path_get_basename(fn);
            Inkscape::IO::remove_file_extension(base);
            if (!base.empty()) {
                initial = std::move(base) + ".nc";
            }
        }
    }

    Glib::RefPtr<Gio::File> const dest = choose_file_save(_("Save G-code as"), win, filters, initial, folder);
    if (!dest) {
        return;
    }
    std::string const path = dest->get_path();
    if (path.empty()) {
        post_status(_("Could not determine a local file path to save."), true);
        return;
    }
    try {
        Glib::file_set_contents(path, text);
    } catch (Glib::FileError const &e) {
        post_status(e.what(), true);
        return;
    }
    if (auto *prefs = Inkscape::Preferences::get()) {
        prefs->setString(k_pref_save_gcode_dir, folder);
    }
    post_status(Glib::ustring::compose(_("Saved G-code to â€?1â€?"), dest->get_parse_name()), false);
}

void GrblControlPanel::on_send_gcode()
{
    if (_gcode_sending.load()) {
        return;
    }
    Glib::RefPtr<Gtk::TextBuffer> const buf = _gcode_view.get_buffer();
    if (!buf) {
        return;
    }
    bool const send_from_cursor = _chk_send_from_cursor_line.get_active();
    guint editor_line_1 = 1;
    std::string text;
    if (send_from_cursor) {
        Glib::RefPtr<Gtk::TextMark> const ins = buf->get_insert();
        Gtk::TextBuffer::iterator const it_mark = buf->get_iter_at_mark(ins);
        editor_line_1 = static_cast<guint>(it_mark.get_line()) + 1;
        Gtk::TextBuffer::iterator const line0 = buf->get_iter_at_line(it_mark.get_line());
        Glib::ustring const partial = buf->get_text(line0, buf->end(), false);
        text = partial.raw();
    } else {
        text = buf->get_text().raw();
    }
    if (!std::any_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); })) {
        post_status(send_from_cursor ? Glib::ustring(_("Nothing to send from the cursor line downward (empty or comments only)."))
                                     : Glib::ustring(_("G-code is empty.")),
                    true);
        return;
    }
    std::size_t const total_exec = count_executable_gcode_lines(text);
    _gcode_sending = true;
    _gcode_cancel = false;
    set_controls_sensitive_for_gcode_stream(false);

    std::thread(
        [this, text = std::move(text), total_exec, send_from_cursor, editor_line_1]() mutable
        {
            std::lock_guard const guard(_port_mutex);
            auto const finish = [this] { finish_gcode_stream_ui(); };

            if (!link_is_open()) {
                post_status(_("Not connected."), true);
                finish();
                return;
            }

            auto pump = [] {
                if (auto const ctx = Glib::MainContext::get_default()) {
                    while (ctx->iteration(false)) {
                    }
                }
            };
            grbl_begin_plot_waits(pump, &_gcode_cancel);
            scope_exit const end_plot{[] { grbl_end_plot_waits(); }};

            if (send_from_cursor) {
                post_status(Glib::ustring::compose(_("Sending G-code starting at editor line %1â€?),
                                                   static_cast<guint64>(editor_line_1)),
                            false);
            }

            using clock = std::chrono::steady_clock;
            clock::time_point last_progress_wall = clock::now();
            std::size_t last_progress_at_sent = 0;
            auto try_send_progress = [&](std::size_t sent, bool force) {
                if (total_exec == 0) {
                    return;
                }
                clock::time_point const now = clock::now();
                std::size_t const span = sent - last_progress_at_sent;
                int const elapsed_ms =
                    static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_wall)
                                         .count());
                if (!force && sent != 1 && span < k_gcode_send_progress_line_stride &&
                    elapsed_ms < k_gcode_send_progress_min_interval_ms) {
                    return;
                }
                if (total_exec <= k_max_gcode_stream_lines) {
                    post_status(Glib::ustring::compose(_("Sending G-code: %1 of %2 linesâ€?), static_cast<guint64>(sent),
                                                       static_cast<guint64>(total_exec)),
                                false);
                } else {
                    post_status(
                        Glib::ustring::compose(
                            _("Sending G-code: line %1 (program exceeds the %2-line limit; send will stop with an "
                              "error)â€?),
                            static_cast<guint64>(sent), static_cast<guint64>(k_max_gcode_stream_lines)),
                        false);
                }
                last_progress_wall = now;
                last_progress_at_sent = sent;
            };

            std::string err;
            std::size_t sent = 0;
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                trim_in_place(line);
                if (should_skip_gcode_line(line)) {
                    continue;
                }
                if (sent >= k_max_gcode_stream_lines) {
                    err = _("Too many G-code lines (limit exceeded).");
                    break;
                }
                if (!link_write_line(line, err)) {
                    if (err == grbl_error_user_cancelled()) {
                        post_status(_("Send stopped (cancel)."), false);
                    } else {
                        post_status(Glib::ustring(Inkscape::Axidraw::grbl_error_to_user_message(err)), true);
                    }
                    finish();
                    return;
                }
                ++sent;
                try_send_progress(sent, false);
            }
            if (!err.empty()) {
                post_status(Glib::ustring(Inkscape::Axidraw::grbl_error_to_user_message(err)), true);
            } else if (sent == 0) {
                post_status(_("No executable lines (only blanks/comments)."), false);
            } else if (send_from_cursor) {
                post_status(Glib::ustring::compose(_("Sent %1 G-code line(s) (started at editor line %2)."),
                                                   static_cast<guint64>(sent), static_cast<guint64>(editor_line_1)),
                            false);
            } else {
                post_status(
                    Glib::ustring::compose(_("Sent %1 G-code line(s)."), static_cast<guint64>(sent)), false);
            }
            finish();
        })
        .detach();
}

void GrblControlPanel::build_ui()
{
    _jog_lbl.set_markup(_("<b>Jog step (mm)</b>"));
    for (char const *v : {"0.01", "0.1", "0.5", "1", "5", "10", "50", "100"}) {
        _jog_dist.append(v);
    }
    _jog_dist.set_active(3);

    _status.set_halign(Gtk::Align::START);
    _status.set_vexpand(false);
    _status.set_wrap(true);
    _status.set_max_width_chars(56);
    _status.set_selectable(true);
    _status.set_text(_("Not connected."));

    _btn_connect.set_label(_("Connect"));
    _btn_connect.set_active(false);
    _btn_connect.set_tooltip_text(
        _("Uses the port selected below (also stored under Edit â†?Preferences â†?Input/Output â†?GRBL pen plotter). "
          "Baud rate comes from the same page."));
    _radio_mode_combo.append("STA", _("WiFi station (STA)"));
    _radio_mode_combo.append("AP", _("WiFi access point (AP)"));
    _radio_mode_combo.append("BT", _("Bluetooth (BT)"));
    _radio_mode_combo.append("OFF", _("Radio off"));
    _radio_mode_combo.set_active_id("STA");
    _radio_mode_combo.set_hexpand(true);
    _radio_mode_combo.set_tooltip_text(
        _("Writes Grbl_ESP32 radio mode via [ESP110]. Options: STA/AP/BT/OFF."));
    _radio_pwd.set_text("admin");
    _radio_pwd.set_visibility(false);
    _radio_pwd.set_placeholder_text(_("admin password"));
    _radio_pwd.set_hexpand(true);
    _radio_pwd.set_tooltip_text(
        _("Admin password used by [ESP110] and optional [ESP444] restart (default is often â€œadminâ€?."));
    _chk_radio_restart.set_label(_("Restart firmware after mode switch ([ESP444])"));
    _chk_radio_restart.set_active(true);
    _chk_radio_restart.set_halign(Gtk::Align::START);
    _chk_radio_restart.set_tooltip_text(
        _("If enabled, send [ESP444]RESTART after [ESP110] so the radio mode is applied immediately."));
    _btn_read_radio_mode.set_tooltip_text(
        _("Query current firmware radio mode via [ESP110]pwd=<password> and sync the selector."));
    _btn_apply_radio_mode.set_tooltip_text(
        _("Send [ESP110]<MODE>pwd=<password> to firmware. Optionally triggers [ESP444]RESTART after switching."));

    _port_lbl.set_halign(Gtk::Align::START);
    _port_lbl.set_valign(Gtk::Align::CENTER);
    _port_lbl.set_markup(_("<b>Port</b>"));
    _port_combo.set_hexpand(true);
    _btn_refresh_ports.set_icon_name("view-refresh-symbolic");
    _btn_refresh_ports.set_tooltip_text(_("Rescan serial ports"));
    _btn_refresh_ports.set_valign(Gtk::Align::CENTER);
    _machine_status.set_halign(Gtk::Align::START);
    _machine_status.set_ellipsize(Pango::EllipsizeMode::END);
    _machine_status.set_max_width_chars(56);
    _machine_status.add_css_class("monospace");
    _machine_status.set_tooltip_text(
        _("Live status from the controller (real-time â€?â€?poll about every 1.5 s while connected)."));

    _frame.set_label(_("GRBL"));
    _frame.set_margin_top(0);
    _frame.set_margin_bottom(0);
    _frame.set_margin_start(0);
    _frame.set_margin_end(0);

    _vbox.set_spacing(10);
    _vbox.set_margin_top(10);
    _vbox.set_margin_bottom(10);
    _vbox.set_margin_start(10);
    _vbox.set_margin_end(10);

    auto *grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(4);
    grid->set_column_spacing(4);
    grid->set_column_homogeneous(true);

    int r = 0;
    grid->attach(_btn_mech_home, 0, r, 1, 1);
    grid->attach(_btn_yp, 1, r, 1, 1);
    grid->attach(_btn_set_origin, 2, r, 1, 1);
    r++;
    grid->attach(_btn_xm, 0, r, 1, 1);
    grid->attach(_btn_goto_work_zero, 1, r, 1, 1);
    grid->attach(_btn_xp, 2, r, 1, 1);
    r++;
    grid->attach(_btn_reset, 0, r, 1, 1);
    grid->attach(_btn_ym, 1, r, 1, 1);
    r++;
    grid->attach(_jog_lbl, 0, r, 1, 1);
    grid->attach(_jog_dist, 1, r, 2, 1);
    r++;
    grid->attach(_btn_pen_up, 0, r, 1, 1);
    grid->attach(_btn_pen_down, 1, r, 1, 1);
    grid->attach(_btn_motors, 2, r, 1, 1);
    r++;
    grid->attach(_btn_clear_alarm, 0, r, 1, 1);

    for (auto *b : {&_btn_mech_home, &_btn_yp, &_btn_set_origin, &_btn_xm, &_btn_goto_work_zero, &_btn_xp, &_btn_reset, &_btn_ym,
                    &_btn_pen_up, &_btn_pen_down, &_btn_motors, &_btn_clear_alarm}) {
        b->set_hexpand(true);
    }
    _jog_dist.set_hexpand(true);

    _btn_mech_home.set_tooltip_text(_("Homing: $H (limit switches and clearance must be set up correctly)."));
    _btn_yp.set_tooltip_text(_("Jog +Y: relative G1, then G90 (absolute) restore."));
    _btn_set_origin.set_tooltip_text(_("G92: set the current position as work zero (X0 Y0 Z0)."));
    _btn_goto_work_zero.set_tooltip_text(_("G90 G0: rapid move to work X0 Y0 in mm (G21)."));
    _btn_goto_work_zero.set_icon_name("go-home-symbolic");
    _btn_xm.set_tooltip_text(_("Jog âˆ’X in millimeters (see jog distance)."));
    _btn_xp.set_tooltip_text(_("Jog +X in millimeters (see jog distance)."));
    _btn_ym.set_tooltip_text(_("Jog âˆ’Y in millimeters (see jog distance)."));
    _btn_reset.set_tooltip_text(_("GRBL soft reset: ASCII 0x18 (Ctrl+X). May require reconnecting."));
    _btn_reset.set_icon_name("view-refresh-symbolic");
    _btn_pen_up.set_tooltip_text(_("Uses the same pen-up method as the plotter (see Preferences / GRBL)."));
    _btn_pen_up.set_icon_name("go-up-symbolic");
    _btn_pen_down.set_tooltip_text(_("Uses the same pen-down method as the plotter (see Preferences / GRBL)."));
    _btn_pen_down.set_icon_name("go-down-symbolic");
    _jog_dist.set_tooltip_text(_("Step size for the X/Y jog buttons."));

    _gcode_frame.set_label(_("Manual G-code"));
    _gcode_help.set_markup(
        _("<small><b>Fill from drawing</b> generates the same program as â€œSend document to GRBL plotterâ€¦â€?(respecting "
          "selection, layer limit, and GRBL preferences) so you can inspect or edit it here before sending. "
          "<b>Load G-code</b> replaces the editor from a file; <b>Save G-code as</b> writes the editor to a file. "
          "<b>Machine-space preview</b> is the primary preview: it maps the final mm plot (after mirror, origin "
          "shift, and optional bed clipping) back to the canvas in orange. "
          "<b>Document-space preview</b> is optional reference geometry before that machine mapping step. "
          "<b>Send from cursor line downward only</b> skips everything above the text cursor (for resuming after an "
          "error). "
          "One command per line; lines starting with <tt>;</tt> and whole-line <tt>(â€?</tt> comments are skipped when "
          "sending. Each line waits for <tt>ok</tt> from the controller. While sending, the log shows line progress. "
          "<b>Cancel</b> applies between lines.</small>"));
    _gcode_help.set_wrap(true);
    _gcode_help.set_halign(Gtk::Align::START);
    _chk_canvas_plot_preview.set_tooltip_text(
        _("Optional reference overlay in document space, using the same sampling and stroke order as export before "
          "millimetre conversion, optional page Y mirror, shifting the plot origin to X0 Y0, and machine-bed "
          "clipping. This is useful for comparing the source geometry against the final machine-space preview."));
    _chk_canvas_plot_preview.set_halign(Gtk::Align::START);
    _chk_machine_space_preview.set_tooltip_text(
        _("Primary preview overlay: the same polylines as G-code after millimetre conversion, optional page Y "
          "mirror, shifting the plot origin to X0 Y0, and optional machine-bed clipping, mapped back into document "
          "units. If bed clipping removed parts of a stroke, the preview becomes approximate in those areas."));
    _chk_machine_space_preview.set_halign(Gtk::Align::START);
    _chk_machine_space_preview.set_active(true);
    _chk_canvas_plot_preview.set_active(false);
    _chk_send_from_cursor_line.set_tooltip_text(
        _("When enabled, â€œSend to machineâ€?streams only from the start of the line containing the text cursor to "
          "the end of the editorâ€”useful after a Grbl error if you delete or skip already-executed lines and place the "
          "cursor on the next command."));
    _chk_send_from_cursor_line.set_halign(Gtk::Align::START);
    _gcode_view.set_accepts_tab(false);
    if (auto const buf = _gcode_view.get_buffer()) {
        buf->set_text("");
    }
    _gcode_view.add_css_class("monospace");
    _gcode_view.set_top_margin(4);
    _gcode_view.set_bottom_margin(4);
    _gcode_view.set_left_margin(4);
    _gcode_view.set_right_margin(4);
    _gcode_view.set_vexpand(true);
    _gcode_scroll.set_child(_gcode_view);
    _gcode_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _gcode_scroll.set_vexpand(true);
    _gcode_scroll.set_min_content_height(120);
    _gcode_scroll.set_has_frame(true);

    _btn_cancel_gcode.set_sensitive(false);
    _btn_load_gcode.set_icon_name("document-open-symbolic");
    _btn_fill_from_drawing.set_icon_name("document-properties-symbolic");
    _btn_save_gcode.set_icon_name("document-save-as-symbolic");
    _btn_send_gcode.set_icon_name("document-send-symbolic");
    _btn_cancel_gcode.set_icon_name("process-stop-symbolic");
    _btn_load_gcode.set_tooltip_text(_("Replace the editor contents from a text file (UTF-8), up to the same size limit as generated jobs."));
    _btn_fill_from_drawing.set_tooltip_text(
        _("Build G-code from the current document using GRBL preferences (same rules as the main â€œSend document to GRBL plotterâ€¦â€?action)."));
    _btn_save_gcode.set_tooltip_text(_("Save the text in the editor to a .nc / .gcode file (UTF-8)."));
    _btn_send_gcode.set_tooltip_text(
        _("Send each non-empty line in order, waiting for a Grbl â€œokâ€?(or an error) before the next line. "
          "The message log updates with approximate line counts during long jobs. "
          "Optional: send only from the cursor line downward (see the checkbox above)."));
    _btn_cancel_gcode.set_tooltip_text(
        _("Set the cancel flag; the current line may still finish before sending stops."));

    Inkscape::UI::pack_start(_gcode_inner, _gcode_help, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_canvas_plot_preview, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_machine_space_preview, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_send_from_cursor_line, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _gcode_scroll, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_load_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_fill_from_drawing, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_save_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_send_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_cancel_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_inner, _gcode_actions, false, false, 2);
    _gcode_frame.set_child(_gcode_inner);

    Inkscape::UI::pack_start(_port_row, _port_lbl, false, false, 6);
    Inkscape::UI::pack_start(_port_row, _port_combo, true, true, 6);
    Inkscape::UI::pack_start(_port_row, _btn_refresh_ports, false, false, 0);

    auto *frame_serial = Gtk::make_managed<Gtk::Frame>();
    frame_serial->set_label(_("Controller link"));
    frame_serial->set_margin_top(0);
    auto *box_serial = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    Inkscape::UI::pack_start(*box_serial, _port_row, false, false, 0);
    auto *radio_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto *radio_lbl = Gtk::make_managed<Gtk::Label>(_("<b>Firmware radio</b>"), Gtk::Align::START);
    radio_lbl->set_use_markup(true);
    Inkscape::UI::pack_start(*radio_row, *radio_lbl, false, false, 0);
    Inkscape::UI::pack_start(*radio_row, _radio_mode_combo, true, true, 0);
    Inkscape::UI::pack_start(*radio_row, _radio_pwd, true, true, 0);
    Inkscape::UI::pack_start(*radio_row, _btn_read_radio_mode, false, false, 0);
    Inkscape::UI::pack_start(*radio_row, _btn_apply_radio_mode, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, *radio_row, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _chk_radio_restart, false, false, 0);
    auto *hdr_status = Gtk::make_managed<Gtk::Label>();
    hdr_status->set_markup(_("<small>Controller</small>"));
    hdr_status->set_halign(Gtk::Align::START);
    Inkscape::UI::pack_start(*box_serial, *hdr_status, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _machine_status, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _btn_connect, false, false, 0);
    frame_serial->set_child(*box_serial);

    auto *frame_motion = Gtk::make_managed<Gtk::Frame>();
    frame_motion->set_label(_("Jog and pen"));
    auto *box_motion = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    box_motion->append(*grid);
    frame_motion->set_child(*box_motion);

    auto *frame_log = Gtk::make_managed<Gtk::Frame>();
    frame_log->set_label(_("Log"));
    auto *box_log = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto *hdr_log = Gtk::make_managed<Gtk::Label>();
    hdr_log->set_markup(_("<small>Messages</small>"));
    hdr_log->set_halign(Gtk::Align::START);
    Inkscape::UI::pack_start(*box_log, *hdr_log, false, false, 0);
    Inkscape::UI::pack_start(*box_log, _status, true, true, 0);
    frame_log->set_child(*box_log);

    Inkscape::UI::pack_start(_vbox, *frame_serial, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *frame_motion, false, false, 0);
    Inkscape::UI::pack_start(_vbox, _gcode_frame, true, true, 0);
    Inkscape::UI::pack_start(_vbox, *frame_log, false, false, 0);
    _frame.set_child(_vbox);
    append(_frame);

    _btn_connect.signal_toggled().connect(sigc::mem_fun(*this, &GrblControlPanel::connect_toggle));
    _btn_refresh_ports.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::refresh_port_list));
    _port_combo.signal_changed().connect(sigc::mem_fun(*this, &GrblControlPanel::on_port_combo_changed));
    _btn_read_radio_mode.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            std::string const pwd = _radio_pwd.get_text();
            if (pwd.empty()) {
                e = _("Admin password is empty.");
                return;
            }
            std::string const cmd = "[ESP110]pwd=" + pwd + "\n";
            if (!link_write_bytes(cmd.data(), cmd.size())) {
                e = _("Could not send radio mode query command.");
                return;
            }
            std::string reply;
            for (int i = 0; i < 8; ++i) {
                std::string line;
                if (!link_read_line(line, 1200)) {
                    break;
                }
                trim_in_place(line);
                if (line.empty() || line == "ok") {
                    continue;
                }
                if (line.rfind("error", 0) == 0 || line.rfind("ERROR", 0) == 0) {
                    e = line;
                    return;
                }
                reply = line;
                break;
            }
            if (reply.empty()) {
                e = _("No radio mode response from firmware.");
                return;
            }
            std::string const mode = detect_radio_mode_from_reply(reply);
            if (mode.empty()) {
                post_status(
                    Glib::ustring::compose(_("Radio mode reply received but not recognized: %1"),
                                           Glib::ustring(reply)),
                    true);
                return;
            }
            Glib::signal_idle().connect_once(sigc::track_object([this, mode] {
                _radio_mode_combo.set_active_id(mode);
            }, *this));
            post_status(Glib::ustring::compose(_("Current firmware radio mode: %1"), Glib::ustring(mode)), false);
        }, false);
    });
    _btn_apply_radio_mode.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            Glib::ustring mode = _radio_mode_combo.get_active_id();
            if (mode.empty()) {
                mode = "STA";
            }
            std::string const pwd = _radio_pwd.get_text();
            if (pwd.empty()) {
                e = _("Admin password is empty.");
                return;
            }
            std::string cmd = "[ESP110]" + mode.raw() + "pwd=" + pwd;
            if (!link_write_line(cmd, e)) {
                return;
            }
            if (_chk_radio_restart.get_active()) {
                std::string restart_cmd = "[ESP444]RESTART pwd=" + pwd;
                std::string restart_err;
                if (!link_write_line(restart_cmd, restart_err)) {
                    post_status(
                        Glib::ustring::compose(
                            _("Radio mode command sent, but restart command failed: %1. You may reconnect manually."),
                            Glib::ustring(restart_err)),
                        true);
                    return;
                }
            }
            Glib::ustring reconnect_hint;
            if (mode == "BT") {
                reconnect_hint = _("Switch your host link to Bluetooth and reconnect.");
            } else if (mode == "AP") {
                reconnect_hint = _("Connect to the controller AP, then use its AP IP (commonly 192.168.0.1 or configured value).");
            } else if (mode == "STA") {
                reconnect_hint = _("Reconnect over your LAN using the controller STA IP/hostname.");
            } else {
                reconnect_hint = _("Radio is off; use wired serial to reconnect.");
            }
            post_status(
                Glib::ustring::compose(
                    _("Radio mode command sent: %1. %2"),
                    mode, reconnect_hint),
                false);
        }, false);
    });

    _btn_mech_home.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!link_write_line("$H", e)) {
                return;
            }
        });
    });
    _btn_yp.signal_clicked().connect([this] { jog_y(+1.0); });
    _btn_ym.signal_clicked().connect([this] { jog_y(-1.0); });
    _btn_xp.signal_clicked().connect([this] { jog_x(+1.0); });
    _btn_xm.signal_clicked().connect([this] { jog_x(-1.0); });
    _btn_set_origin.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!link_write_line("G21", e)) {
                return;
            }
            if (!link_write_line("G92 X0 Y0 Z0", e)) {
                return;
            }
        });
    });
    _btn_goto_work_zero.signal_clicked().connect([this] {
        run_action(
            [this](std::string &e) {
                if (!link_write_line("G21", e)) {
                    return;
                }
                if (!link_write_line("G90", e)) {
                    return;
                }
                if (!link_write_line("G0 X0 Y0", e)) {
                    return;
                }
            });
    });
    _btn_reset.signal_clicked().connect([this] { soft_reset(); });
    _btn_pen_up.signal_clicked().connect([this] { send_pen_state(true); });
    _btn_pen_down.signal_clicked().connect([this] { send_pen_state(false); });
    _btn_motors.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!link_write_line("$SLP", e)) {
                return;
            }
        });
    });
    _btn_clear_alarm.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!link_write_line("$X", e)) {
                return;
            }
        });
    });
    _chk_canvas_plot_preview.signal_toggled().connect(sigc::mem_fun(*this, &GrblControlPanel::sync_plot_preview_overlay));
    _chk_machine_space_preview.signal_toggled().connect(
        sigc::mem_fun(*this, &GrblControlPanel::sync_plot_preview_overlay));
    _btn_load_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_load_gcode_from_file));
    _btn_fill_from_drawing.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_fill_gcode_from_document));
    _btn_save_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_save_gcode_as));
    _btn_send_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_send_gcode));
    _btn_cancel_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_cancel_gcode_stream));
}

} // namespace Inkscape::UI::Dialog

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
