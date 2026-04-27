// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-mapping-state.h"

using Inkscape::UI::Dialog::get_grbl_page_restore_button_sensitive;
using Inkscape::UI::Dialog::get_grbl_panel_mapping_refresh_preview;
using Inkscape::UI::Dialog::GrblPanelMappingRefreshKind;
using Inkscape::UI::Dialog::make_grbl_panel_editor_gcode_change_plan;
using Inkscape::UI::Dialog::make_grbl_panel_mapping_change_plan;
using Inkscape::UI::Dialog::make_grbl_panel_mapping_sensitivity;

TEST(GrblPanelMappingStateTest, SensitivityTracksMasterAndDependentControls)
{
    auto const state =
        make_grbl_panel_mapping_sensitivity(true, true, false, true, false, true, false, false, true, false, true, false, false);

    EXPECT_TRUE(state.swap_xy);
    EXPECT_TRUE(state.bed_width);
    EXPECT_TRUE(state.bed_depth);
    EXPECT_FALSE(state.long_pen_up_height);
    EXPECT_FALSE(state.long_move_dist);
    EXPECT_TRUE(state.near_connect_dist);
    EXPECT_TRUE(state.manual_pen_change_to_home);
    EXPECT_TRUE(state.manual_pen_change_prompt);
    EXPECT_FALSE(state.tool_change_point);
    EXPECT_FALSE(state.tool_change_x);
}

TEST(GrblPanelMappingStateTest, RestoreButtonFollowsSavedStateAndStreaming)
{
    EXPECT_TRUE(get_grbl_page_restore_button_sensitive(true, false));
    EXPECT_FALSE(get_grbl_page_restore_button_sensitive(false, false));
    EXPECT_FALSE(get_grbl_page_restore_button_sensitive(true, true));
}

TEST(GrblPanelMappingStateTest, MappingChangeWarnsOnlyOnceAndRefreshesWhenIdle)
{
    auto const first = make_grbl_panel_mapping_change_plan(false, true, false, false, true);
    EXPECT_TRUE(first.allow_interaction);
    EXPECT_TRUE(first.refresh_plot_feedback);
    EXPECT_TRUE(first.warn_stale_generated_gcode);
    EXPECT_TRUE(first.stale_warning_should_be_marked_posted);

    auto const repeated = make_grbl_panel_mapping_change_plan(false, true, false, true, true);
    EXPECT_FALSE(repeated.warn_stale_generated_gcode);

    auto const busy = make_grbl_panel_mapping_change_plan(true, true, false, false, true);
    EXPECT_FALSE(busy.allow_interaction);
    EXPECT_FALSE(busy.refresh_plot_feedback);
}

TEST(GrblPanelMappingStateTest, EditorGcodeChangesClearTrackingOnlyForManualEdits)
{
    auto const manual = make_grbl_panel_editor_gcode_change_plan(false, false, true);
    EXPECT_TRUE(manual.clear_generation_state);
    EXPECT_TRUE(manual.reset_stale_warning_latch);
    EXPECT_TRUE(manual.schedule_plot_feedback_refresh);
    EXPECT_TRUE(manual.refresh_preview);

    auto const generated = make_grbl_panel_editor_gcode_change_plan(true, false, false);
    EXPECT_FALSE(generated.clear_generation_state);
    EXPECT_FALSE(generated.reset_stale_warning_latch);
    EXPECT_TRUE(generated.schedule_plot_feedback_refresh);
    EXPECT_FALSE(generated.refresh_preview);
}

TEST(GrblPanelMappingStateTest, MappingRefreshClassificationSeparatesSummaryOnlyFromOverlayChanges)
{
    EXPECT_TRUE(get_grbl_panel_mapping_refresh_preview(GrblPanelMappingRefreshKind::geometry_projection));
    EXPECT_TRUE(get_grbl_panel_mapping_refresh_preview(GrblPanelMappingRefreshKind::path_processing));
    EXPECT_TRUE(get_grbl_panel_mapping_refresh_preview(GrblPanelMappingRefreshKind::bed_size));

    EXPECT_FALSE(get_grbl_panel_mapping_refresh_preview(GrblPanelMappingRefreshKind::motion_timing));
    EXPECT_FALSE(get_grbl_panel_mapping_refresh_preview(GrblPanelMappingRefreshKind::pen_motion_commands));
    EXPECT_FALSE(get_grbl_panel_mapping_refresh_preview(GrblPanelMappingRefreshKind::tool_change_behavior));
    EXPECT_FALSE(get_grbl_panel_mapping_refresh_preview(GrblPanelMappingRefreshKind::custom_gcode_blocks));
}
