// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-mapping-state.h"

namespace Inkscape::UI::Dialog {

GrblPanelMappingSensitivity make_grbl_panel_mapping_sensitivity(bool const allow_interaction, bool const clip_bed_active,
                                                                bool const lead_in_active, bool const lead_out_active,
                                                                bool const contour_to_hatch_active,
                                                                bool const hatch_inset_active,
                                                                bool const hatch_angle_increment_active,
                                                                bool const long_pen_up_active,
                                                                bool const near_connect_active,
                                                                bool const sparse_sampling_active,
                                                                bool const manual_tool_change_mode,
                                                                bool const m6_tool_change_mode,
                                                                bool const tool_change_point_active)
{
    GrblPanelMappingSensitivity state;
    state.swap_xy = allow_interaction;
    state.invert_x = allow_interaction;
    state.invert_y = allow_interaction;
    state.flip_y = allow_interaction;
    state.align_origin = allow_interaction;
    state.clip_bed = allow_interaction;
    state.lead_in = allow_interaction;
    state.lead_out = allow_interaction;
    state.contour_to_hatch = allow_interaction;
    state.hatch_cross = allow_interaction && contour_to_hatch_active;
    state.hatch_inset_enable = allow_interaction && contour_to_hatch_active;
    state.hatch_angle_increment_enable = allow_interaction && contour_to_hatch_active;
    state.long_pen_up = allow_interaction;
    state.near_connect = allow_interaction;
    state.sparse_sampling = allow_interaction;
    state.tool_change_mode = allow_interaction;
    state.draw_feed = allow_interaction;
    state.travel_feed = allow_interaction;
    state.pen_up_delay = allow_interaction;
    state.pen_down_delay = allow_interaction;
    state.lead_in_dist = allow_interaction && lead_in_active;
    state.lead_out_dist = allow_interaction && lead_out_active;
    state.hatch_spacing = allow_interaction && contour_to_hatch_active;
    state.hatch_angle = allow_interaction && contour_to_hatch_active;
    state.hatch_inset = allow_interaction && contour_to_hatch_active && hatch_inset_active;
    state.hatch_angle_increment = allow_interaction && contour_to_hatch_active && hatch_angle_increment_active;
    state.pen_up_cmd = allow_interaction;
    state.pen_down_cmd = allow_interaction;
    state.manual_pen_change_to_home = allow_interaction && manual_tool_change_mode;
    state.manual_pen_change_prompt = allow_interaction && manual_tool_change_mode;
    state.tool_change_point = allow_interaction && m6_tool_change_mode;
    state.bed_width = allow_interaction && clip_bed_active;
    state.bed_depth = allow_interaction && clip_bed_active;
    state.long_pen_up_height = allow_interaction && long_pen_up_active;
    state.long_move_dist = allow_interaction && long_pen_up_active;
    state.near_connect_dist = allow_interaction && near_connect_active;
    state.sparse_keep_every = allow_interaction && sparse_sampling_active;
    state.tool_change_x = allow_interaction && m6_tool_change_mode && tool_change_point_active;
    state.tool_change_y = allow_interaction && m6_tool_change_mode && tool_change_point_active;
    return state;
}

bool get_grbl_page_restore_button_sensitive(bool const has_saved_page_restore, bool const gcode_active)
{
    return has_saved_page_restore && !gcode_active;
}

GrblPanelMappingChangePlan make_grbl_panel_mapping_change_plan(bool const gcode_active,
                                                               bool const has_generated_gcode_state,
                                                               bool const mapping_matches_generated_gcode,
                                                               bool const stale_warning_already_posted,
                                                               bool const refresh_preview)
{
    GrblPanelMappingChangePlan plan;
    plan.allow_interaction = !gcode_active;
    plan.refresh_plot_feedback = plan.allow_interaction && refresh_preview;

    if (!has_generated_gcode_state || mapping_matches_generated_gcode) {
        return plan;
    }

    if (!stale_warning_already_posted) {
        plan.warn_stale_generated_gcode = true;
        plan.stale_warning_should_be_marked_posted = true;
    }
    return plan;
}

GrblPanelEditorGcodeChangePlan make_grbl_panel_editor_gcode_change_plan(bool const generated_from_document,
                                                                        bool const suspend_tracking,
                                                                        bool const preview_enabled)
{
    GrblPanelEditorGcodeChangePlan plan;
    plan.clear_generation_state = !generated_from_document && !suspend_tracking;
    plan.reset_stale_warning_latch = plan.clear_generation_state;
    plan.schedule_plot_feedback_refresh = true;
    plan.refresh_preview = preview_enabled;
    return plan;
}

} // namespace Inkscape::UI::Dialog
