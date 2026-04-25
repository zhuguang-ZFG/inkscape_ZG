// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Internal GRBL panel sender helpers extracted from the panel UI unit.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_SENDER_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_SENDER_H

#include <cstddef>
#include <mutex>
#include <string>

#include <glib.h>

class SPDesktop;
class SPDocument;

namespace Gtk {
class Window;
}

namespace Inkscape {
class Selection;
namespace Axidraw {
struct GrblExportParams;
}
namespace UI::Dialog {

class GrblControlPanel;

class GrblPanelSender {
public:
    static void run_direct_send_worker(GrblControlPanel &panel, std::unique_lock<std::mutex> &port_lock,
                                       SPDocument *doc, SPDesktop *desktop, Selection *selection,
                                       bool use_current_layer_without_selection,
                                       Axidraw::GrblExportParams params, Gtk::Window *win);
    static void run_editor_gcode_send_worker(GrblControlPanel &panel, std::unique_lock<std::mutex> &port_lock,
                                             std::string text, std::size_t total_exec, bool send_from_cursor,
                                             guint editor_line_1);
};

} // namespace UI::Dialog
} // namespace Inkscape

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_SENDER_H
