// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared GRBL mapping/preferences persistence helpers for the control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_MAPPING_PREFS_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_MAPPING_PREFS_H

#include <string>

namespace Inkscape {
class Preferences;
}

namespace Inkscape::UI::Dialog {

struct GrblPanelMappingPrefs
{
    bool sync_page_to_bed = true;
    bool swap_xy = false;
    bool invert_x = false;
    bool invert_y = false;
    bool flip_y = false;
    bool align_origin = false;
    bool clip_bed = true;
    bool lead_in = false;
    bool lead_out = false;
    bool long_pen_up = false;
    bool near_connect = false;
    bool sparse_sampling = false;
    bool contour_to_hatch = false;
    bool hatch_cross = false;
    bool manual_pen_change_to_home = true;
    bool manual_pen_change_prompt = true;
    bool tool_change_point = false;
    bool auto_pause_between_layers = false;
    bool manual_pen_change = false;
    bool tool_change_m6 = false;
    double draw_feed = 1200.0;
    double travel_feed = 6000.0;
    double pen_up_delay = 0.0;
    double pen_down_delay = 0.0;
    double lead_in_dist = 0.0;
    double lead_out_dist = 0.0;
    double hatch_spacing = 1.0;
    double hatch_angle = 0.0;
    std::string pen_up_cmd = "G1 Z0 F3000";
    std::string pen_down_cmd = "G1 Z5 F3000";
    double bed_width = 300.0;
    double bed_depth = 200.0;
    double long_pen_up_height = 10.0;
    double long_move_dist = 20.0;
    double near_connect_dist = 0.3;
    int sparse_keep_every = 1;
    double tool_change_x = 0.0;
    double tool_change_y = 0.0;
    std::string start_gcode;
    std::string end_gcode;
};

char const *get_grbl_tool_change_mode_id(bool auto_pause_between_layers, bool manual_pen_change, bool tool_change_m6);
void apply_grbl_tool_change_mode_id(std::string const &mode_id, GrblPanelMappingPrefs &prefs);
GrblPanelMappingPrefs load_grbl_panel_mapping_prefs(Inkscape::Preferences &prefs);
void save_grbl_panel_mapping_prefs(Inkscape::Preferences &prefs, GrblPanelMappingPrefs const &values);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_MAPPING_PREFS_H
