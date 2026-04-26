// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared firmware-sync orchestration helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_STATE_H

#include <glibmm/ustring.h>

#include "ui/dialog/grbl-panel-firmware-sync.h"
#include "ui/dialog/grbl-runtime-state.h"

namespace Inkscape::UI::Dialog {

struct GrblPanelFirmwareSyncUiPlan
{
    bool save_mapping_preferences = false;
    bool schedule_plot_feedback_refresh = false;
    Glib::ustring status;
};

struct GrblFirmwareMappingUpdate
{
    bool has_invert_x = false;
    bool invert_x = false;
    bool has_invert_y = false;
    bool invert_y = false;
    bool has_bed_width = false;
    double bed_width = 0.0;
    bool has_bed_depth = false;
    double bed_depth = 0.0;
};

enum class GrblFirmwareSyncRequestOrigin {
    manual,
    connect_success,
    delayed_connect,
};

struct GrblFirmwareSyncRequestPlan
{
    bool start_now = false;
    bool keep_delayed_request = false;
};

bool can_start_grbl_firmware_sync(GrblRuntimePhase phase);
GrblFirmwareSyncRequestPlan make_grbl_firmware_sync_request_plan(GrblFirmwareSyncRequestOrigin origin,
                                                                 bool connect_active, GrblRuntimePhase phase);
GrblFirmwareMappingUpdate make_grbl_firmware_mapping_update(GrblFirmwareSnapshot const &snapshot);
Glib::ustring build_grbl_firmware_sync_status(GrblFirmwareSnapshot const &snapshot, bool page_synced, bool unit_synced);
GrblPanelFirmwareSyncUiPlan make_grbl_firmware_sync_ui_plan(GrblFirmwareSnapshot const &snapshot,
                                                            GrblFirmwareSyncApplyResult const &apply_result);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_STATE_H
