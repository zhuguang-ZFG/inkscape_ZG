// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-firmware-sync-state.h"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {
namespace {

std::string trim_ascii_whitespace(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

} // namespace

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
        notes.emplace_back(_("未从参数中读到 $130 / $131 行程参数，因此没有同步页面尺寸。"));
    }
    if (page_synced) {
        notes.emplace_back(Glib::ustring::compose(_("已将当前页面尺寸同步为 %1 x %2 mm"),
                                                  snapshot.x_travel_mm, snapshot.y_travel_mm));
    }
    if (unit_synced) {
        notes.emplace_back(_("已将文档单位同步为 mm"));
    }

    auto const summary = snapshot.imported_from_file
        ? Glib::ustring(_("已从参数文件导入绘图机参数。"))
        : Glib::ustring(_("已读取固件参数。"));
    if (notes.empty()) {
        return summary;
    }

    std::ostringstream msg;
    msg << summary.raw();
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

bool parse_grbl_properties_snapshot(std::string const &text, GrblFirmwareSnapshot &snapshot, Glib::ustring &error)
{
    snapshot = GrblFirmwareSnapshot{};
    snapshot.imported_from_file = true;

    std::istringstream input(text);
    std::string line;
    std::vector<std::string> setting_lines;
    bool saw_setting = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto trimmed = trim_ascii_whitespace(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '!') {
            continue;
        }

        std::string key_part = trimmed;
        if (!key_part.empty() && key_part.front() == '$') {
            key_part.erase(key_part.begin());
        }

        auto const eq = key_part.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= key_part.size()) {
            continue;
        }

        auto const key = trim_ascii_whitespace(key_part.substr(0, eq));
        auto const value = trim_ascii_whitespace(key_part.substr(eq + 1));
        if (key.empty() || value.empty()) {
            continue;
        }

        int code = 0;
        try {
            size_t idx = 0;
            code = std::stoi(key, &idx);
            if (idx != key.size()) {
                continue;
            }
        } catch (...) {
            continue;
        }

        saw_setting = true;
        setting_lines.push_back("$" + std::to_string(code) + "=" + value);

        try {
            if (code == 3) {
                size_t idx = 0;
                int const parsed = std::stoi(value, &idx);
                if (idx == value.size()) {
                    snapshot.direction_mask = parsed;
                    snapshot.has_direction_mask = true;
                }
            } else if (code == 130) {
                size_t idx = 0;
                double const parsed = std::stod(value, &idx);
                if (idx == value.size()) {
                    snapshot.x_travel_mm = parsed;
                    snapshot.has_x_travel = true;
                }
            } else if (code == 131) {
                size_t idx = 0;
                double const parsed = std::stod(value, &idx);
                if (idx == value.size()) {
                    snapshot.y_travel_mm = parsed;
                    snapshot.has_y_travel = true;
                }
            }
        } catch (...) {
        }
    }

    if (!saw_setting) {
        error = _("所选文件里没有找到可识别的 GRBL 参数项。");
        return false;
    }

    std::ostringstream out;
    out << "[imported $$]\n";
    for (auto const &entry : setting_lines) {
        out << entry << "\n";
    }
    snapshot.display_text = out.str();
    return true;
}

} // namespace Inkscape::UI::Dialog
