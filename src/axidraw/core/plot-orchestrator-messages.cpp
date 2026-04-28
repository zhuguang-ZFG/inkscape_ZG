// SPDX-License-Identifier: GPL-2.0-or-later

#include "plot-orchestrator-messages.h"

#include <glibmm/i18n.h>
#include <glibmm/ustring.h>

#include "axidraw/device/grbl-client.h"

namespace Inkscape::Axidraw {

std::string describe_probe_failure(Glib::ustring const &device, int const baud, GrblProbeResult const &probe)
{
    if (probe.response_line.empty()) {
        return Glib::ustring::compose(
                   _("No GRBL status response on %1 at %2 baud. Check the serial port, baud rate, and controller power."),
                   device, baud)
            .raw();
    }
    return Glib::ustring::compose(
               _("The selected serial port %1 (%2 baud) answered, but not like a GRBL controller:\n%3"), device, baud,
               Glib::ustring(probe.response_line))
        .raw();
}

std::string describe_open_failure(Glib::ustring const &device, int const baud, bool const timed_out)
{
    if (timed_out) {
        return Glib::ustring::compose(
                   _("Timed out while opening serial port %1 at %2 baud. Check whether another program is holding the "
                     "port, whether the USB/serial driver is responsive, and whether the controller is powered."),
                   device, baud)
            .raw();
    }
    return Glib::ustring::compose(_("Could not open serial port %1 at %2 baud."), device, baud).raw();
}

} // namespace Inkscape::Axidraw
