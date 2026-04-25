// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Internal GRBL panel sender helpers extracted from the panel UI unit.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_SENDER_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_SENDER_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>

#include <glib.h>
#include <glibmm/ustring.h>

class SPDesktop;
class SPDocument;

namespace Gtk {
class Window;
}

namespace Inkscape {
class Selection;
namespace Axidraw {
class GrblLink;
struct GrblExportParams;
}
namespace UI::Dialog {

struct GrblPanelSenderContext {
    Axidraw::GrblLink *link = nullptr;
    std::atomic<bool> const *cancel = nullptr;
    std::function<void(Glib::ustring const &, bool)> post_status;
    std::function<void()> post_not_connected_status;
    std::function<void(std::string const &)> post_gcode_stream_result;
    std::function<void()> refresh_plot_feedback_after_gcode_change;
    std::function<void(std::unique_lock<std::mutex> &)> finish_worker;
    std::function<void(std::function<void()>)> with_plot_waits;
};

class GrblPanelSender {
public:
    static void run_direct_send_worker(GrblPanelSenderContext const &context, std::unique_lock<std::mutex> &port_lock,
                                       SPDocument *doc, SPDesktop *desktop, Inkscape::Selection *selection,
                                       bool use_current_layer_without_selection,
                                       Axidraw::GrblExportParams const &params, Gtk::Window *win);
    static void run_editor_gcode_send_worker(GrblPanelSenderContext const &context, std::unique_lock<std::mutex> &port_lock,
                                             std::string text, std::size_t total_exec, bool send_from_cursor,
                                             guint editor_line_1);
};

} // namespace UI::Dialog
} // namespace Inkscape

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_SENDER_H
