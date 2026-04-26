// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-transport-state.h"

namespace Inkscape::UI::Dialog {

GrblPanelTransportPlan make_transport_plan_for_firmware_sync_start(bool const has_link)
{
    GrblPanelTransportPlan plan;
    plan.activity = has_link ? GrblPanelLinkActivityState::firmware_sync : GrblPanelLinkActivityState::unchanged;
    plan.update_poll = true;
    plan.poll_enabled = false;
    return plan;
}

GrblPanelTransportPlan make_transport_plan_for_firmware_sync_complete(bool const has_link, bool const resume_poll,
                                                                      bool const connect_active)
{
    GrblPanelTransportPlan plan;
    plan.activity = has_link ? GrblPanelLinkActivityState::idle : GrblPanelLinkActivityState::unchanged;
    plan.update_poll = resume_poll && connect_active;
    plan.poll_enabled = true;
    return plan;
}

GrblPanelTransportPlan make_transport_plan_for_gcode_stream(bool const active, bool const has_link,
                                                            bool const connect_active, bool const firmware_syncing)
{
    GrblPanelTransportPlan plan;
    if (active) {
        plan.activity = has_link ? GrblPanelLinkActivityState::streaming : GrblPanelLinkActivityState::unchanged;
        plan.update_poll = true;
        plan.poll_enabled = false;
        return plan;
    }

    if (firmware_syncing) {
        return plan;
    }

    plan.activity = has_link ? GrblPanelLinkActivityState::idle : GrblPanelLinkActivityState::unchanged;
    plan.update_poll = connect_active;
    plan.poll_enabled = connect_active;
    return plan;
}

} // namespace Inkscape::UI::Dialog
