// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * User-facing GRBL connection failure message helpers for plot orchestrator.
 */
#ifndef INK_AXIDRAW_PLOT_ORCHESTRATOR_MESSAGES_H
#define INK_AXIDRAW_PLOT_ORCHESTRATOR_MESSAGES_H

#include <string>

namespace Glib {
class ustring;
}

namespace Inkscape::Axidraw {

struct GrblProbeResult;

std::string describe_probe_failure(Glib::ustring const &device, int baud, GrblProbeResult const &probe);
std::string describe_open_failure(Glib::ustring const &device, int baud, bool timed_out);

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_PLOT_ORCHESTRATOR_MESSAGES_H
