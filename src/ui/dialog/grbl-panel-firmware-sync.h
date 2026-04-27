// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Internal GRBL panel firmware sync helpers extracted from the panel UI unit.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_H

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <glibmm/ustring.h>

namespace Inkscape::Axidraw {
class GrblLink;
}

namespace Inkscape::UI::Dialog {

struct GrblFirmwareSnapshot {
    bool has_direction_mask = false;
    int direction_mask = 0;
    bool has_x_travel = false;
    bool has_y_travel = false;
    double x_travel_mm = 0.0;
    double y_travel_mm = 0.0;
    bool has_esp_ip = false;
    std::string esp_ip;
    bool has_esp_data_port = false;
    int esp_data_port = 0;
    bool has_esp_hostname = false;
    std::string esp_hostname;
    bool has_esp_wifi_mode = false;
    std::string esp_wifi_mode;
    Glib::ustring display_text;
};

struct GrblFirmwareSyncApplyResult {
    bool changed = false;
    bool page_synced = false;
    bool unit_synced = false;
};

struct GrblPanelFirmwareSyncContext {
    Inkscape::Axidraw::GrblLink *link = nullptr;
    std::function<bool(std::atomic<bool> const &, std::function<void()>)> with_locked_open_link;
    std::function<void(std::function<void()>)> dispatch_to_ui;
    std::function<void()> finish_sync_ui;
    std::function<GrblFirmwareSyncApplyResult(GrblFirmwareSnapshot const &)> apply_snapshot_to_ui;
    std::string esp_admin_password;
    std::function<void(Glib::ustring const &)> set_firmware_info_text;
    std::function<void(bool)> save_mapping_preferences;
    std::function<void(bool)> schedule_plot_feedback_refresh;
    std::function<void(Glib::ustring const &, bool)> post_status;
};

class GrblPanelFirmwareSync {
public:
    static void run(GrblPanelFirmwareSyncContext const &context, std::atomic<bool> const &stop);
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_H
