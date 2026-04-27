// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-mapping-prefs.h"

#include <cmath>

#include "preferences.h"

namespace Inkscape::UI::Dialog {
namespace {

constexpr auto k_default_end_gcode = "G0 X0 Y0";
constexpr auto k_default_bed_preset = "A4";
constexpr auto k_pref_draw = "/options/grbl/feed-draw-mmmin";
constexpr auto k_pref_travel = "/options/grbl/feed-travel-mmmin";
constexpr auto k_pref_pen_up = "/options/grbl/pen-up-cmd";
constexpr auto k_pref_pen_down = "/options/grbl/pen-down-cmd";
constexpr auto k_pref_pen_up_delay = "/options/grbl/pen-up-delay-ms";
constexpr auto k_pref_pen_down_delay = "/options/grbl/pen-down-delay-ms";
constexpr auto k_pref_sync_page_to_bed = "/options/grbl/sync-page-to-bed-on-firmware-read";
constexpr auto k_pref_swap_xy = "/options/grbl/swap-xy";
constexpr auto k_pref_invert_x = "/options/grbl/invert-x";
constexpr auto k_pref_invert_y = "/options/grbl/invert-y";
constexpr auto k_pref_flip_y = "/options/grbl/flip-y-canvas";
constexpr auto k_pref_align_origin = "/options/grbl/align-content-min";
constexpr auto k_pref_clip_bed = "/options/grbl/clip-to-machine-bed";
constexpr auto k_pref_lead_in = "/options/grbl/enable-path-lead-in";
constexpr auto k_pref_lead_in_dist = "/options/grbl/path-lead-in-distance-mm";
constexpr auto k_pref_lead_out = "/options/grbl/enable-path-lead-out";
constexpr auto k_pref_lead_out_dist = "/options/grbl/path-lead-out-distance-mm";
constexpr auto k_pref_lead_in_out = "/options/grbl/enable-path-lead-in-out";
constexpr auto k_pref_lead_in_out_dist = "/options/grbl/path-lead-in-out-distance-mm";
constexpr auto k_pref_bed_preset = "/options/grbl/machine-bed-preset";
constexpr auto k_pref_bed_width = "/options/grbl/machine-bed-width-mm";
constexpr auto k_pref_bed_depth = "/options/grbl/machine-bed-depth-mm";
constexpr auto k_pref_long_pen_up = "/options/grbl/enable-long-pen-up";
constexpr auto k_pref_long_pen_up_mm = "/options/grbl/long-pen-up-mm";
constexpr auto k_pref_long_move_dist = "/options/grbl/long-move-dist-mm";
constexpr auto k_pref_near_connect = "/options/grbl/enable-near-connect";
constexpr auto k_pref_near_connect_dist = "/options/grbl/near-connect-distance-mm";
constexpr auto k_pref_sparse_sampling = "/options/grbl/enable-sparse-stroke-sampling";
constexpr auto k_pref_sparse_keep_every = "/options/grbl/sparse-keep-every";
constexpr auto k_pref_contour_to_hatch = "/options/grbl/contour-to-hatch";
constexpr auto k_pref_hatch_spacing = "/options/grbl/hatch-spacing-mm";
constexpr auto k_pref_hatch_angle = "/options/grbl/hatch-angle-deg";
constexpr auto k_pref_hatch_cross = "/options/grbl/hatch-cross";
constexpr auto k_pref_hatch_inset_enable = "/options/grbl/hatch-inset-enable";
constexpr auto k_pref_hatch_inset = "/options/grbl/hatch-inset-mm";
constexpr auto k_pref_hatch_angle_increment_enable = "/options/grbl/hatch-angle-increment-enable";
constexpr auto k_pref_hatch_angle_increment = "/options/grbl/hatch-angle-increment-deg";
constexpr auto k_pref_auto_pause_between_layers = "/options/grbl/auto-pause-between-layers";
constexpr auto k_pref_manual_pen_change = "/options/grbl/manual-pen-change";
constexpr auto k_pref_pen_change_to_home = "/options/grbl/pen-change-to-home";
constexpr auto k_pref_pen_change_prompt = "/options/grbl/pen-change-prompt";
constexpr auto k_pref_tool_change_m6 = "/options/grbl/enable-layer-tool-change-m6";
constexpr auto k_pref_tool_change_point = "/options/grbl/tool-change-use-point";
constexpr auto k_pref_tool_change_x = "/options/grbl/tool-change-x-mm";
constexpr auto k_pref_tool_change_y = "/options/grbl/tool-change-y-mm";
constexpr auto k_pref_start_gcode = "/options/grbl/start-gcode";
constexpr auto k_pref_end_gcode = "/options/grbl/end-gcode";
constexpr auto k_pref_end_gcode_migration_v1 = "/options/grbl/migrations/end-gcode-default-v1";

void migrate_end_gcode_default(Inkscape::Preferences &prefs)
{
    if (prefs.getEntry(k_pref_end_gcode_migration_v1).isSet()) {
        return;
    }

    auto end_gcode = prefs.getString(k_pref_end_gcode);
    if (end_gcode.find_first_not_of(" \t\r\n") == std::string::npos) {
        prefs.setString(k_pref_end_gcode, k_default_end_gcode);
    }
    prefs.setBool(k_pref_end_gcode_migration_v1, true);
    prefs.save();
}

std::string normalize_end_gcode(std::string value)
{
    return value;
}

constexpr GrblBedPresetInfo k_bed_presets[] = {
    {"A0", 841.0, 1189.0},
    {"A1", 594.0, 841.0},
    {"A2", 420.0, 594.0},
    {"A3", 297.0, 420.0},
    {"A4", 210.0, 297.0},
    {"A5", 148.0, 210.0},
    {"A6", 105.0, 148.0},
    {"B0", 1000.0, 1414.0},
    {"B1", 728.0, 1030.0},
    {"B2", 515.0, 728.0},
    {"B3", 364.0, 515.0},
    {"B4", 250.0, 353.0},
    {"B5", 176.0, 250.0},
    {"C4", 229.0, 324.0},
    {"C5", 162.0, 229.0},
    {"C6", 114.0, 162.0},
    {"16K", 210.0, 285.0},
    {"Letter", 215.9, 279.4},
    {"Legal", 215.9, 355.6},
};

bool nearly_equal_mm(double const a, double const b, double const epsilon = 0.05)
{
    return std::abs(a - b) <= epsilon;
}

bool is_legacy_auto_bed_size(double const width_mm, double const depth_mm)
{
    return (nearly_equal_mm(width_mm, 300.0) && nearly_equal_mm(depth_mm, 200.0)) ||
           (nearly_equal_mm(width_mm, 200.0) && nearly_equal_mm(depth_mm, 200.0));
}

} // namespace

std::span<GrblBedPresetInfo const> get_grbl_bed_preset_definitions()
{
    return k_bed_presets;
}

bool lookup_grbl_bed_preset_dimensions(std::string const &id, double &width_mm, double &depth_mm)
{
    for (auto const &preset : k_bed_presets) {
        if (id == preset.id) {
            width_mm = preset.width_mm;
            depth_mm = preset.depth_mm;
            return true;
        }
    }
    return false;
}

std::string infer_grbl_bed_preset_id(double const width_mm, double const depth_mm)
{
    for (auto const &preset : k_bed_presets) {
        if (nearly_equal_mm(width_mm, preset.width_mm) && nearly_equal_mm(depth_mm, preset.depth_mm)) {
            return preset.id;
        }
    }
    return "custom";
}

GrblPanelMappingPrefs load_grbl_panel_mapping_prefs(Inkscape::Preferences &prefs)
{
    migrate_end_gcode_default(prefs);
    GrblPanelMappingPrefs values;
    bool prefs_changed = false;
    values.sync_page_to_bed = prefs.getBool(k_pref_sync_page_to_bed, true);
    values.swap_xy = prefs.getBool(k_pref_swap_xy, false);
    values.invert_x = prefs.getBool(k_pref_invert_x, false);
    values.invert_y = prefs.getBool(k_pref_invert_y, false);
    values.flip_y = prefs.getBool(k_pref_flip_y, false);
    values.align_origin = prefs.getBool(k_pref_align_origin, false);
    values.clip_bed = prefs.getBool(k_pref_clip_bed, true);
    auto const legacy_lead_enabled = prefs.getBool(k_pref_lead_in_out, false);
    auto const legacy_lead_dist = prefs.getDoubleLimited(k_pref_lead_in_out_dist, 0.0, 0.0, 1000.0);
    values.lead_in = prefs.getBool(k_pref_lead_in, legacy_lead_enabled);
    values.lead_out = prefs.getBool(k_pref_lead_out, legacy_lead_enabled);
    values.long_pen_up = prefs.getBool(k_pref_long_pen_up, false);
    values.near_connect = prefs.getBool(k_pref_near_connect, false);
    values.sparse_sampling = prefs.getBool(k_pref_sparse_sampling, false);
    values.contour_to_hatch = prefs.getBool(k_pref_contour_to_hatch, false);
    values.hatch_cross = prefs.getBool(k_pref_hatch_cross, false);
    values.hatch_inset_enable = prefs.getBool(k_pref_hatch_inset_enable, false);
    values.hatch_angle_increment_enable = prefs.getBool(k_pref_hatch_angle_increment_enable, false);
    values.auto_pause_between_layers = prefs.getBool(k_pref_auto_pause_between_layers, false);
    values.manual_pen_change = prefs.getBool(k_pref_manual_pen_change, false);
    values.tool_change_m6 = prefs.getBool(k_pref_tool_change_m6, false);
    values.manual_pen_change_to_home = prefs.getBool(k_pref_pen_change_to_home, true);
    values.manual_pen_change_prompt = prefs.getBool(k_pref_pen_change_prompt, true);
    values.tool_change_point = prefs.getBool(k_pref_tool_change_point, false);
    values.draw_feed = prefs.getDoubleLimited(k_pref_draw, 1200.0, 60.0, 12000.0);
    values.travel_feed = prefs.getDoubleLimited(k_pref_travel, 6000.0, 60.0, 20000.0);
    values.pen_up_delay = prefs.getDoubleLimited(k_pref_pen_up_delay, 0.0, 0.0, 5000.0);
    values.pen_down_delay = prefs.getDoubleLimited(k_pref_pen_down_delay, 0.0, 0.0, 5000.0);
    values.lead_in_dist = prefs.getDoubleLimited(k_pref_lead_in_dist, legacy_lead_dist, 0.0, 1000.0);
    values.lead_out_dist = prefs.getDoubleLimited(k_pref_lead_out_dist, legacy_lead_dist, 0.0, 1000.0);
    values.hatch_spacing = prefs.getDoubleLimited(k_pref_hatch_spacing, 1.0, 0.05, 100.0);
    values.hatch_angle = prefs.getDoubleLimited(k_pref_hatch_angle, 0.0, -180.0, 180.0);
    values.hatch_inset = prefs.getDoubleLimited(k_pref_hatch_inset, 0.1, 0.0, 100.0);
    values.hatch_angle_increment = prefs.getDoubleLimited(k_pref_hatch_angle_increment, 5.0, -180.0, 180.0);
    values.pen_up_cmd = prefs.getString(k_pref_pen_up, "G90\nG1 Z0 F1200");
    values.pen_down_cmd = prefs.getString(k_pref_pen_down, "G90\nG1 Z5 F1200");
    values.bed_width = prefs.getDoubleLimited(k_pref_bed_width, 210.0, 1.0, 2000.0);
    values.bed_depth = prefs.getDoubleLimited(k_pref_bed_depth, 297.0, 1.0, 2000.0);
    values.bed_preset = prefs.getString(k_pref_bed_preset, "");
    if (values.bed_preset.empty()) {
        values.bed_preset = infer_grbl_bed_preset_id(values.bed_width, values.bed_depth);
        if (values.bed_preset == "custom" && is_legacy_auto_bed_size(values.bed_width, values.bed_depth)) {
            values.bed_preset = k_default_bed_preset;
            lookup_grbl_bed_preset_dimensions(values.bed_preset, values.bed_width, values.bed_depth);
        }
        prefs.setString(k_pref_bed_preset, values.bed_preset);
        prefs.setDouble(k_pref_bed_width, values.bed_width);
        prefs.setDouble(k_pref_bed_depth, values.bed_depth);
        prefs_changed = true;
    } else if (values.bed_preset != "custom") {
        double preset_width = 0.0;
        double preset_depth = 0.0;
        if (lookup_grbl_bed_preset_dimensions(values.bed_preset, preset_width, preset_depth)) {
            if (!nearly_equal_mm(values.bed_width, preset_width) || !nearly_equal_mm(values.bed_depth, preset_depth)) {
                values.bed_width = preset_width;
                values.bed_depth = preset_depth;
                prefs.setDouble(k_pref_bed_width, values.bed_width);
                prefs.setDouble(k_pref_bed_depth, values.bed_depth);
                prefs_changed = true;
            } else {
                values.bed_width = preset_width;
                values.bed_depth = preset_depth;
            }
        } else {
            values.bed_preset = infer_grbl_bed_preset_id(values.bed_width, values.bed_depth);
            prefs.setString(k_pref_bed_preset, values.bed_preset);
            prefs_changed = true;
        }
    }
    values.long_pen_up_height = prefs.getDoubleLimited(k_pref_long_pen_up_mm, 10.0, -1000.0, 1000.0);
    values.long_move_dist = prefs.getDoubleLimited(k_pref_long_move_dist, 20.0, 0.0, 100000.0);
    values.near_connect_dist = prefs.getDoubleLimited(k_pref_near_connect_dist, 0.3, 0.0, 1000.0);
    values.sparse_keep_every = prefs.getIntLimited(k_pref_sparse_keep_every, 1, 1, 64);
    values.tool_change_x = prefs.getDouble(k_pref_tool_change_x);
    values.tool_change_y = prefs.getDouble(k_pref_tool_change_y);
    values.start_gcode = prefs.getString(k_pref_start_gcode, "");
    values.end_gcode = normalize_end_gcode(prefs.getString(k_pref_end_gcode, k_default_end_gcode));
    if (prefs_changed) {
        prefs.save();
    }
    return values;
}

void save_grbl_panel_mapping_prefs(Inkscape::Preferences &prefs, GrblPanelMappingPrefs const &values)
{
    prefs.setBool(k_pref_sync_page_to_bed, values.sync_page_to_bed);
    prefs.setBool(k_pref_swap_xy, values.swap_xy);
    prefs.setBool(k_pref_invert_x, values.invert_x);
    prefs.setBool(k_pref_invert_y, values.invert_y);
    prefs.setBool(k_pref_flip_y, values.flip_y);
    prefs.setBool(k_pref_align_origin, values.align_origin);
    prefs.setBool(k_pref_clip_bed, values.clip_bed);
    prefs.setBool(k_pref_lead_in, values.lead_in);
    prefs.setBool(k_pref_lead_out, values.lead_out);
    prefs.setBool(k_pref_lead_in_out, values.lead_in && values.lead_out);
    prefs.setBool(k_pref_long_pen_up, values.long_pen_up);
    prefs.setBool(k_pref_near_connect, values.near_connect);
    prefs.setBool(k_pref_sparse_sampling, values.sparse_sampling);
    prefs.setBool(k_pref_contour_to_hatch, values.contour_to_hatch);
    prefs.setBool(k_pref_hatch_cross, values.hatch_cross);
    prefs.setBool(k_pref_hatch_inset_enable, values.hatch_inset_enable);
    prefs.setBool(k_pref_hatch_angle_increment_enable, values.hatch_angle_increment_enable);
    prefs.setBool(k_pref_auto_pause_between_layers, values.auto_pause_between_layers);
    prefs.setBool(k_pref_manual_pen_change, values.manual_pen_change);
    prefs.setBool(k_pref_pen_change_to_home, values.manual_pen_change_to_home);
    prefs.setBool(k_pref_pen_change_prompt, values.manual_pen_change_prompt);
    prefs.setBool(k_pref_tool_change_m6, values.tool_change_m6);
    prefs.setBool(k_pref_tool_change_point, values.tool_change_point);
    prefs.setDouble(k_pref_draw, values.draw_feed);
    prefs.setDouble(k_pref_travel, values.travel_feed);
    prefs.setDouble(k_pref_pen_up_delay, values.pen_up_delay);
    prefs.setDouble(k_pref_pen_down_delay, values.pen_down_delay);
    prefs.setDouble(k_pref_lead_in_dist, values.lead_in_dist);
    prefs.setDouble(k_pref_lead_out_dist, values.lead_out_dist);
    prefs.setDouble(k_pref_hatch_spacing, values.hatch_spacing);
    prefs.setDouble(k_pref_hatch_angle, values.hatch_angle);
    prefs.setDouble(k_pref_hatch_inset, values.hatch_inset);
    prefs.setDouble(k_pref_hatch_angle_increment, values.hatch_angle_increment);
    prefs.setDouble(k_pref_lead_in_out_dist, (values.lead_in_dist + values.lead_out_dist) * 0.5);
    prefs.setString(k_pref_pen_up, values.pen_up_cmd);
    prefs.setString(k_pref_pen_down, values.pen_down_cmd);
    prefs.setString(k_pref_bed_preset, values.bed_preset.empty() ? infer_grbl_bed_preset_id(values.bed_width, values.bed_depth)
                                                                  : values.bed_preset);
    prefs.setDouble(k_pref_bed_width, values.bed_width);
    prefs.setDouble(k_pref_bed_depth, values.bed_depth);
    prefs.setDouble(k_pref_long_pen_up_mm, values.long_pen_up_height);
    prefs.setDouble(k_pref_long_move_dist, values.long_move_dist);
    prefs.setDouble(k_pref_near_connect_dist, values.near_connect_dist);
    prefs.setInt(k_pref_sparse_keep_every, values.sparse_keep_every);
    prefs.setDouble(k_pref_tool_change_x, values.tool_change_x);
    prefs.setDouble(k_pref_tool_change_y, values.tool_change_y);
    prefs.setString(k_pref_start_gcode, values.start_gcode);
    prefs.setString(k_pref_end_gcode, values.end_gcode);
    prefs.save();
}

} // namespace Inkscape::UI::Dialog
