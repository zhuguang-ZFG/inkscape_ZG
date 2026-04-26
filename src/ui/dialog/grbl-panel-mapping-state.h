// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared mapping/preferences interaction helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_MAPPING_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_MAPPING_STATE_H

namespace Inkscape::UI::Dialog {

struct GrblPanelMappingSensitivity
{
    bool swap_xy = false;
    bool invert_x = false;
    bool invert_y = false;
    bool flip_y = false;
    bool align_origin = false;
    bool clip_bed = false;
    bool long_pen_up = false;
    bool near_connect = false;
    bool sparse_sampling = false;
    bool tool_change_mode = false;
    bool draw_feed = false;
    bool travel_feed = false;
    bool pen_up_delay = false;
    bool pen_down_delay = false;
    bool pen_up_cmd = false;
    bool pen_down_cmd = false;
    bool manual_pen_change_to_home = false;
    bool manual_pen_change_prompt = false;
    bool tool_change_point = false;
    bool bed_width = false;
    bool bed_depth = false;
    bool long_pen_up_height = false;
    bool long_move_dist = false;
    bool near_connect_dist = false;
    bool sparse_keep_every = false;
    bool tool_change_x = false;
    bool tool_change_y = false;
};

struct GrblPanelMappingChangePlan
{
    bool allow_interaction = false;
    bool refresh_plot_feedback = false;
    bool warn_stale_generated_gcode = false;
    bool stale_warning_should_be_marked_posted = false;
};

struct GrblPanelEditorGcodeChangePlan
{
    bool clear_generation_state = false;
    bool reset_stale_warning_latch = false;
    bool schedule_plot_feedback_refresh = false;
    bool refresh_preview = false;
};

GrblPanelMappingSensitivity make_grbl_panel_mapping_sensitivity(bool allow_interaction, bool clip_bed_active,
                                                                bool long_pen_up_active, bool near_connect_active,
                                                                bool sparse_sampling_active, bool manual_tool_change_mode,
                                                                bool m6_tool_change_mode, bool tool_change_point_active);
bool get_grbl_page_restore_button_sensitive(bool has_saved_page_restore, bool gcode_active);
GrblPanelMappingChangePlan make_grbl_panel_mapping_change_plan(bool gcode_active, bool has_generated_gcode_state,
                                                               bool mapping_matches_generated_gcode,
                                                               bool stale_warning_already_posted, bool refresh_preview);
GrblPanelEditorGcodeChangePlan make_grbl_panel_editor_gcode_change_plan(bool generated_from_document,
                                                                        bool suspend_tracking, bool preview_enabled);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_MAPPING_STATE_H
