// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared connection lifecycle helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_CONNECTION_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_CONNECTION_STATE_H

#include "ui/dialog/grbl-runtime-state.h"

namespace Inkscape::UI::Dialog {

enum class GrblPanelConnectButtonAction {
    none,
    deactivate,
};

struct GrblPanelConnectionUiPlan
{
    bool set_connecting = false;
    bool cancel_delayed_firmware_sync = false;
    bool update_poll = false;
    bool poll_enabled = false;
    bool clear_machine_status = false;
    bool post_status = false;
    bool status_is_error = false;
    bool refresh_plot_feedback = false;
    bool trigger_firmware_sync_now = false;
    bool schedule_delayed_firmware_sync = false;
    bool disconnect_link = false;
    GrblPanelConnectButtonAction connect_button = GrblPanelConnectButtonAction::none;
};

GrblPanelConnectionUiPlan make_connect_attempt_begin_plan();
GrblPanelConnectionUiPlan make_disconnect_controller_plan();
GrblPanelConnectionUiPlan make_connect_attempt_failure_plan(bool connect_active, bool clear_machine_status);
GrblPanelConnectionUiPlan make_connect_attempt_success_plan(bool connect_active);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_CONNECTION_STATE_H
