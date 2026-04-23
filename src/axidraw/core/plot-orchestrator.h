// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * GRBL plot orchestration (native AxiDraw migration).
 */
#ifndef INK_AXIDRAW_PLOT_ORCHESTRATOR_H
#define INK_AXIDRAW_PLOT_ORCHESTRATOR_H

#include <string>

namespace Gtk {
class Window;
} // namespace Gtk

class SPDocument;
class SPDesktop;

namespace Inkscape::Axidraw {

enum class GrblConnectAttempt {
    Ok,
    Failed,
    Cancelled,
};

/**
 * Resolve serial settings, verify the selected controller answers like GRBL, then stream the current
 * document through the native GRBL export pipeline while reporting status back to the caller.
 */
class PlotOrchestrator
{
public:
    static GrblConnectAttempt run_plot_grbl(SPDocument *doc, SPDesktop *desktop, Gtk::Window &parent,
                                             std::string &message_out);
};

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_PLOT_ORCHESTRATOR_H
