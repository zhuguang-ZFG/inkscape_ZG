// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared job-entry guard helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_JOB_GUARD_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_JOB_GUARD_H

#include <glibmm/ustring.h>

#include "ui/dialog/grbl-editor-gcode.h"

namespace Inkscape::UI::Dialog {

struct GrblEditorSendGuardInput
{
    bool send_from_cursor = false;
    bool has_executable_content = false;
    bool stale_generated_gcode = false;
    bool check_bed_bounds = false;
    EditorGcodeBounds bounds;
    double bed_width_mm = 0.0;
    double bed_height_mm = 0.0;
};

enum class GrblDirectSendConnectionState {
    ready_serial,
    not_connected,
    tcp_connected,
};

struct GrblDirectSendGuardInput
{
    GrblDirectSendConnectionState connection_state = GrblDirectSendConnectionState::ready_serial;
    bool requires_parent_window = false;
    bool has_parent_window = true;
};

Glib::ustring get_grbl_editor_send_block_reason(GrblEditorSendGuardInput const &input);
Glib::ustring get_grbl_direct_send_block_reason(GrblDirectSendGuardInput const &input);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_JOB_GUARD_H
