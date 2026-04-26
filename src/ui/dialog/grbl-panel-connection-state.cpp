// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-connection-state.h"

namespace Inkscape::UI::Dialog {

GrblPanelConnectionUiPlan make_connect_attempt_begin_plan()
{
    GrblPanelConnectionUiPlan plan;
    plan.set_connecting = true;
    plan.cancel_delayed_firmware_sync = true;
    plan.update_poll = true;
    plan.poll_enabled = false;
    plan.post_status = true;
    return plan;
}

GrblPanelConnectionUiPlan make_disconnect_controller_plan()
{
    GrblPanelConnectionUiPlan plan;
    plan.cancel_delayed_firmware_sync = true;
    plan.update_poll = true;
    plan.poll_enabled = false;
    plan.clear_machine_status = true;
    return plan;
}

GrblPanelConnectionUiPlan make_connect_attempt_failure_plan(bool const connect_active,
                                                            bool const clear_machine_status)
{
    GrblPanelConnectionUiPlan plan;
    plan.clear_machine_status = connect_active && clear_machine_status;
    if (!connect_active) {
        return plan;
    }

    plan.connect_button = GrblPanelConnectButtonAction::deactivate;
    plan.post_status = true;
    plan.status_is_error = true;
    return plan;
}

GrblPanelConnectionUiPlan make_connect_attempt_success_plan(bool const connect_active)
{
    GrblPanelConnectionUiPlan plan;
    if (!connect_active) {
        plan.disconnect_link = true;
        return plan;
    }

    plan.post_status = true;
    plan.update_poll = true;
    plan.poll_enabled = true;
    plan.refresh_plot_feedback = true;
    plan.trigger_firmware_sync_now = true;
    plan.cancel_delayed_firmware_sync = true;
    plan.schedule_delayed_firmware_sync = true;
    return plan;
}

} // namespace Inkscape::UI::Dialog
