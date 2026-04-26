// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared presentation helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_PRESENTATION_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_PRESENTATION_H

#include <string>

#include <glibmm/ustring.h>

#include "ui/dialog/grbl-runtime-state.h"

namespace Inkscape::UI::Dialog {

struct GrblButtonPresentation
{
    Glib::ustring label;
    Glib::ustring tooltip;
};

struct GrblActionButtonsPresentation
{
    GrblButtonPresentation send_gcode;
    GrblButtonPresentation send_from_drawing;
    GrblButtonPresentation read_firmware;
    GrblButtonPresentation cancel_gcode;
};

struct GrblConnectionPresentation
{
    bool serial_controls_sensitive = true;
    GrblButtonPresentation connect;
};

GrblActionButtonsPresentation make_grbl_action_buttons_presentation(GrblRuntimeStateView const &state);
GrblConnectionPresentation make_grbl_connection_presentation(GrblRuntimeStateView const &state, bool connected);
Glib::ustring build_grbl_connection_status(Glib::ustring const &device, Glib::ustring const &probe_response = {});
Glib::ustring build_grbl_not_connected_status(bool serial_required);
bool grbl_gcode_text_has_content(std::string const &text);
std::string build_grbl_default_gcode_filename(char const *document_filename);
Glib::ustring build_grbl_gcode_load_status(Glib::ustring const &parse_name);
Glib::ustring build_grbl_gcode_save_status(Glib::ustring const &parse_name);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_PRESENTATION_H
