// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared preview/feedback presentation helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_PRESENTATION_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_PRESENTATION_H

#include <cstddef>
#include <string>

#include <glibmm/ustring.h>

namespace Inkscape::UI::Dialog {

struct GrblPreviewOverlayUiPlan
{
    bool build_machine_axis = false;
    bool post_status = false;
    bool request_canvas_redraw = false;
    Glib::ustring status;
};

Glib::ustring build_grbl_preview_build_error_status(std::string const &err, bool machine_space);
Glib::ustring build_grbl_preview_status_note(bool machine_space, std::size_t included, std::size_t total,
                                             bool clip_approx);
GrblPreviewOverlayUiPlan make_grbl_preview_overlay_ui_plan(bool machine_preview_active,
                                                           Glib::ustring const &status_note);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_PRESENTATION_H
