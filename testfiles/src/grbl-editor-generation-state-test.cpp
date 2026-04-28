// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-editor-generation-state.h"

using Inkscape::UI::Dialog::EditorGcodeGenerationInputs;
using Inkscape::UI::Dialog::EditorGcodeGenerationState;
using Inkscape::UI::Dialog::build_editor_gcode_stale_mapping_message;
using Inkscape::UI::Dialog::editor_gcode_generation_context_matches;
using Inkscape::UI::Dialog::editor_gcode_generation_state_matches;
using Inkscape::UI::Dialog::make_editor_gcode_generation_inputs;
using Inkscape::UI::Dialog::make_editor_gcode_generation_state;

TEST(GrblEditorGenerationStateTest, BuildsStateFromExportParams)
{
    auto const state =
        make_editor_gcode_generation_state(true, true, false, true, true, "lower_left", 320.0, 180.0);

    EXPECT_TRUE(state.valid);
    EXPECT_TRUE(state.clip_to_machine_bed);
    EXPECT_TRUE(state.swap_xy);
    EXPECT_FALSE(state.invert_x);
    EXPECT_TRUE(state.invert_y);
    EXPECT_TRUE(state.flip_y_canvas);
    EXPECT_EQ(state.plot_anchor, "lower_left");
    EXPECT_DOUBLE_EQ(state.machine_bed_width_mm, 320.0);
    EXPECT_DOUBLE_EQ(state.machine_bed_depth_mm, 180.0);
}

TEST(GrblEditorGenerationStateTest, MatchesOnlyWhenMappingInputsStayTheSame)
{
    EditorGcodeGenerationState state;
    state.valid = true;
    state.clip_to_machine_bed = true;
    state.swap_xy = false;
    state.invert_x = true;
    state.invert_y = false;
    state.flip_y_canvas = true;
    state.plot_anchor = "none";
    state.machine_bed_width_mm = 300.0;
    state.machine_bed_depth_mm = 200.0;

    auto const same = make_editor_gcode_generation_inputs(true, false, true, false, true, "none", 300.0, 200.0);
    auto const changed =
        make_editor_gcode_generation_inputs(true, true, true, false, true, "none", 300.0, 200.0);

    EXPECT_TRUE(editor_gcode_generation_state_matches(state, same));
    EXPECT_FALSE(editor_gcode_generation_state_matches(state, changed));
}

TEST(GrblEditorGenerationStateTest, InvalidStateNeverMatches)
{
    EditorGcodeGenerationState state;
    auto const inputs = make_editor_gcode_generation_inputs(false, false, false, false, false, "none", 300.0, 200.0);

    EXPECT_FALSE(editor_gcode_generation_state_matches(state, inputs));
    EXPECT_FALSE(editor_gcode_generation_context_matches(state, inputs, true));
}

TEST(GrblEditorGenerationStateTest, ContextMatchesOnlyWhenDocumentAndMappingStillMatch)
{
    auto const state = make_editor_gcode_generation_state(true, false, true, false, true, "none", 300.0, 200.0);
    auto const same = make_editor_gcode_generation_inputs(true, false, true, false, true, "none", 300.0, 200.0);
    auto const changed =
        make_editor_gcode_generation_inputs(true, true, true, false, true, "none", 300.0, 200.0);

    EXPECT_TRUE(editor_gcode_generation_context_matches(state, same, true));
    EXPECT_FALSE(editor_gcode_generation_context_matches(state, same, false));
    EXPECT_FALSE(editor_gcode_generation_context_matches(state, changed, true));
}

TEST(GrblEditorGenerationStateTest, BuildsStaleMappingMessage)
{
    auto const message = build_editor_gcode_stale_mapping_message().raw();

    EXPECT_NE(message.find("G-code"), std::string::npos);
    EXPECT_FALSE(message.empty());
}
