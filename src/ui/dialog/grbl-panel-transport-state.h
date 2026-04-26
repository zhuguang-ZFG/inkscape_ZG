// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared transport-state transition helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_TRANSPORT_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_TRANSPORT_STATE_H

namespace Inkscape::UI::Dialog {

enum class GrblPanelLinkActivityState {
    unchanged,
    idle,
    firmware_sync,
    streaming,
};

struct GrblPanelTransportPlan
{
    GrblPanelLinkActivityState activity = GrblPanelLinkActivityState::unchanged;
    bool update_poll = false;
    bool poll_enabled = false;
};

GrblPanelTransportPlan make_transport_plan_for_firmware_sync_start(bool has_link);
GrblPanelTransportPlan make_transport_plan_for_firmware_sync_complete(bool has_link, bool resume_poll,
                                                                      bool connect_active);
GrblPanelTransportPlan make_transport_plan_for_gcode_stream(bool active, bool has_link, bool connect_active,
                                                            bool firmware_syncing);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_TRANSPORT_STATE_H
