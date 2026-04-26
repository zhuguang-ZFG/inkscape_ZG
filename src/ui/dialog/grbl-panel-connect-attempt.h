// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared connect-attempt worker helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_CONNECT_ATTEMPT_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_CONNECT_ATTEMPT_H

#include <atomic>
#include <memory>
#include <string>

#include <glibmm/ustring.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/serial-port.h"
#include "axidraw/device/tcp-port.h"

namespace Inkscape::UI::Dialog {

struct GrblPanelConnectRequest
{
    Glib::ustring device;
    int baud = 115200;
    std::string tcp_host;
    int tcp_port = 0;

    bool use_tcp() const { return !tcp_host.empty() && tcp_port > 0; }
};

enum class GrblPanelConnectAttemptOutcome {
    cancelled,
    open_failed,
    probe_failed,
    connected,
};

struct GrblPanelConnectAttemptResult
{
    GrblPanelConnectAttemptOutcome outcome = GrblPanelConnectAttemptOutcome::cancelled;
    std::unique_ptr<Inkscape::Axidraw::SerialPort> serial;
    std::unique_ptr<Inkscape::Axidraw::TcpPort> tcp;
    bool serial_open_timed_out = false;
    bool serial_open_access_denied = false;
    std::string probe_response_line;
};

bool parse_tcp_device_spec(std::string const &spec, std::string &host_out, int &port_out);
Glib::ustring describe_probe_failure_ui(Glib::ustring const &device, int baud,
                                        Inkscape::Axidraw::GrblProbeResult const &probe);
Glib::ustring describe_tcp_probe_failure_ui(Glib::ustring const &device,
                                            Inkscape::Axidraw::GrblProbeResult const &probe);
Glib::ustring describe_connect_open_failure_ui(bool use_tcp, bool timed_out, bool access_denied = false);
Glib::ustring make_connect_probe_status(Glib::ustring const &device, int baud, bool use_tcp);
int serial_open_timeout_for_device(std::string const &device);

class GrblPanelConnectAttempt {
public:
    static GrblPanelConnectAttemptResult run(GrblPanelConnectRequest const &request, std::atomic<bool> const &stop);
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_CONNECT_ATTEMPT_H
