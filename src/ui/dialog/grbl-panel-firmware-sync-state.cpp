// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-firmware-sync-state.h"

#include <sstream>
#include <vector>

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {

bool can_start_grbl_firmware_sync(GrblRuntimePhase const phase)
{
    return phase != GrblRuntimePhase::gcode_sending &&
           phase != GrblRuntimePhase::gcode_cancelling;
}

GrblFirmwareSyncRequestPlan make_grbl_firmware_sync_request_plan(GrblFirmwareSyncRequestOrigin const origin,
                                                                 bool const connect_active,
                                                                 GrblRuntimePhase const phase)
{
    GrblFirmwareSyncRequestPlan plan;
    switch (origin) {
        case GrblFirmwareSyncRequestOrigin::manual:
            plan.start_now = can_start_grbl_firmware_sync(phase);
            return plan;
        case GrblFirmwareSyncRequestOrigin::connect_success:
            if (!connect_active) {
                return plan;
            }
            plan.start_now = can_start_grbl_firmware_sync(phase);
            plan.keep_delayed_request = true;
            return plan;
        case GrblFirmwareSyncRequestOrigin::delayed_connect:
            if (!connect_active || phase == GrblRuntimePhase::connecting) {
                return plan;
            }
            plan.start_now = can_start_grbl_firmware_sync(phase);
            return plan;
        default:
            return plan;
    }
}

GrblFirmwareMappingUpdate make_grbl_firmware_mapping_update(GrblFirmwareSnapshot const &snapshot)
{
    GrblFirmwareMappingUpdate update;
    if (snapshot.has_direction_mask) {
        update.has_invert_x = true;
        update.invert_x = (snapshot.direction_mask & 0x1) != 0;
        update.has_invert_y = true;
        update.invert_y = (snapshot.direction_mask & 0x2) != 0;
    }
    if (snapshot.has_x_travel) {
        update.has_bed_width = true;
        update.bed_width = snapshot.x_travel_mm;
    }
    if (snapshot.has_y_travel) {
        update.has_bed_depth = true;
        update.bed_depth = snapshot.y_travel_mm;
    }
    return update;
}

Glib::ustring build_grbl_firmware_sync_status(GrblFirmwareSnapshot const &snapshot, bool const page_synced,
                                              bool const unit_synced)
{
    std::vector<Glib::ustring> notes;
    if (snapshot.has_direction_mask) {
        notes.emplace_back(Glib::ustring::compose(_("已同步方向反转掩码 $3=%1"), snapshot.direction_mask));
    }
    if (snapshot.has_x_travel || snapshot.has_y_travel) {
        if (snapshot.has_x_travel && snapshot.has_y_travel) {
            notes.emplace_back(Glib::ustring::compose(_("已同步床面尺寸 X=%1 mm, Y=%2 mm"),
                                                      snapshot.x_travel_mm, snapshot.y_travel_mm));
        } else if (snapshot.has_x_travel) {
            notes.emplace_back(Glib::ustring::compose(_("已同步床面宽度 X=%1 mm"), snapshot.x_travel_mm));
        } else {
            notes.emplace_back(Glib::ustring::compose(_("已同步床面深度 Y=%1 mm"), snapshot.y_travel_mm));
        }
    } else {
        notes.emplace_back(_("未从固件读取到 $130 / $131 行程参数，因此没有同步页面尺寸。"));
    }
    if (page_synced) {
        notes.emplace_back(Glib::ustring::compose(_("已将当前页面尺寸同步为 %1 x %2 mm"),
                                                  snapshot.x_travel_mm, snapshot.y_travel_mm));
    }
    if (unit_synced) {
        notes.emplace_back(_("已将文档单位同步为 mm"));
    }
    if (notes.empty()) {
        return _("已读取固件参数。");
    }

    std::ostringstream msg;
    msg << _("已读取固件参数。");
    for (auto const &note : notes) {
        msg << "\n" << note.raw();
    }
    return Glib::ustring(msg.str());
}

GrblPanelFirmwareSyncUiPlan make_grbl_firmware_sync_ui_plan(GrblFirmwareSnapshot const &snapshot,
                                                            GrblFirmwareSyncApplyResult const &apply_result)
{
    GrblPanelFirmwareSyncUiPlan plan;
    plan.save_mapping_preferences = apply_result.changed;
    plan.schedule_plot_feedback_refresh = !apply_result.changed;
    plan.status = build_grbl_firmware_sync_status(snapshot, apply_result.page_synced, apply_result.unit_synced);
    return plan;
}

} // namespace Inkscape::UI::Dialog
