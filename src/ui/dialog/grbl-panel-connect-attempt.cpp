// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-connect-attempt.h"

#include <chrono>
#include <thread>

#include <glibmm/i18n.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/serial-port.h"
#include "axidraw/device/tcp-port.h"

namespace Inkscape::UI::Dialog {

bool parse_tcp_device_spec(std::string const &spec, std::string &host_out, int &port_out)
{
    std::string s = spec;
    if (s.rfind("tcp://", 0) == 0) {
        s.erase(0, 6);
    }

    std::string port_text;
    if (!s.empty() && s.front() == '[') {
        auto const closing = s.find(']');
        if (closing == std::string::npos || closing <= 1 || closing + 2 >= s.size() || s[closing + 1] != ':') {
            return false;
        }
        host_out = s.substr(1, closing - 1);
        port_text = s.substr(closing + 2);
    } else {
        auto const colon = s.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
            return false;
        }
        if (s.find(':') != colon) {
            return false;
        }
        host_out = s.substr(0, colon);
        port_text = s.substr(colon + 1);
    }

    try {
        int const p = std::stoi(port_text);
        if (p <= 0 || p > 65535) {
            return false;
        }
        port_out = p;
        return !host_out.empty();
    } catch (...) {
        return false;
    }
}

Glib::ustring describe_probe_failure_ui(Glib::ustring const &device, int const baud,
                                        Inkscape::Axidraw::GrblProbeResult const &probe)
{
    if (probe.response_line.empty()) {
        return Glib::ustring::compose(
            _("在 %1（%2 波特）上没有收到 GRBL 响应。请检查串口、波特率以及控制器供电。"), device, baud);
    }
    return Glib::ustring::compose(_("串口 %1（%2 波特）已有响应，但看起来不像 GRBL 控制器：\n%3"), device, baud,
                                  Glib::ustring(probe.response_line));
}

Glib::ustring describe_tcp_probe_failure_ui(Glib::ustring const &device,
                                            Inkscape::Axidraw::GrblProbeResult const &probe)
{
    auto const detail = probe.response_line.empty()
                            ? Glib::ustring(_("TCP 上没有收到 GRBL 响应。"))
                            : Glib::ustring::compose(_("TCP 端点已有响应，但看起来不像 GRBL：\n%1"),
                                                     Glib::ustring(probe.response_line));
    return Glib::ustring::compose(_("无法连接到 %1。\n%2"), device, detail);
}

Glib::ustring describe_connect_open_failure_ui(bool const use_tcp, bool const timed_out, bool const access_denied)
{
    if (!use_tcp && access_denied) {
        return _("无法打开所选串口连接：系统拒绝访问该端口。它通常表示串口正被其他程序、另一个 Inkscape 进程或蓝牙串口服务占用。");
    }
    if (!use_tcp && timed_out) {
        return _("打开所选串口连接超时。请检查串口是否被其他程序占用、驱动是否正常，以及控制器是否已上电。");
    }
    return use_tcp ? Glib::ustring(_("无法打开所选 TCP 连接。")) : Glib::ustring(_("无法打开所选串口连接。"));
}

Glib::ustring make_connect_probe_status(Glib::ustring const &device, int const baud, bool const use_tcp)
{
    return use_tcp ? Glib::ustring::compose(_("正在通过 TCP 探测 %1..."), device)
                   : Glib::ustring::compose(_("正在以 %2 波特探测 %1..."), device, baud);
}

int serial_open_timeout_for_device(std::string const &device)
{
    if (device.size() >= 3 && device.rfind("COM", 0) == 0) {
        return 12000;
    }
    return 6000;
}

GrblPanelConnectAttemptResult GrblPanelConnectAttempt::run(GrblPanelConnectRequest const &request,
                                                           std::atomic<bool> const &stop)
{
    GrblPanelConnectAttemptResult result;
    if (stop.load(std::memory_order_acquire)) {
        return result;
    }

    result.serial = std::make_unique<Inkscape::Axidraw::SerialPort>();
    result.tcp = std::make_unique<Inkscape::Axidraw::TcpPort>();

    int const serial_open_timeout_ms = serial_open_timeout_for_device(request.device.raw());
    bool const opened = request.use_tcp()
        ? result.tcp->open(request.tcp_host, request.tcp_port)
        : result.serial->open(request.device.raw(), request.baud, serial_open_timeout_ms);
    if (!request.use_tcp() && !opened && result.serial->last_open_timed_out() && !stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!stop.load(std::memory_order_acquire)) {
            (void)result.serial->open(request.device.raw(), request.baud, serial_open_timeout_ms);
        }
    }

    if (stop.load(std::memory_order_acquire)) {
        result.outcome = GrblPanelConnectAttemptOutcome::cancelled;
        return result;
    }

    bool const is_open = request.use_tcp() ? result.tcp->is_open() : result.serial->is_open();
    if (!is_open) {
        result.outcome = GrblPanelConnectAttemptOutcome::open_failed;
        result.serial_open_timed_out = !request.use_tcp() && result.serial->last_open_timed_out();
        result.serial_open_access_denied = !request.use_tcp() && result.serial->last_open_access_denied();
        return result;
    }

    auto const probe = request.use_tcp()
        ? Inkscape::Axidraw::probe_open_grbl(*result.tcp)
        : Inkscape::Axidraw::probe_open_grbl(*result.serial);
    if (stop.load(std::memory_order_acquire)) {
        result.outcome = GrblPanelConnectAttemptOutcome::cancelled;
        return result;
    }

    result.probe_response_line = probe.response_line;
    result.outcome = probe.ok ? GrblPanelConnectAttemptOutcome::connected : GrblPanelConnectAttemptOutcome::probe_failed;
    return result;
}

} // namespace Inkscape::UI::Dialog
