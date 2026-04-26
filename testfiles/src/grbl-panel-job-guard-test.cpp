// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-editor-generation-state.h"
#include "src/ui/dialog/grbl-panel-job-guard.h"

using Inkscape::UI::Dialog::GrblDirectSendConnectionState;
using Inkscape::UI::Dialog::GrblDirectSendGuardInput;
using Inkscape::UI::Dialog::GrblEditorSendGuardInput;
using Inkscape::UI::Dialog::build_editor_gcode_out_of_bed_message;
using Inkscape::UI::Dialog::build_editor_gcode_stale_mapping_message;
using Inkscape::UI::Dialog::get_grbl_direct_send_block_reason;
using Inkscape::UI::Dialog::get_grbl_editor_send_block_reason;

TEST(GrblPanelJobGuardTest, EmptyEditorSendMessageDependsOnCursorMode)
{
    GrblEditorSendGuardInput input;
    input.send_from_cursor = true;

    auto const cursor_reason = get_grbl_editor_send_block_reason(input);
    EXPECT_FALSE(cursor_reason.empty());

    input.send_from_cursor = false;
    auto const normal_reason = get_grbl_editor_send_block_reason(input);
    EXPECT_FALSE(normal_reason.empty());
    EXPECT_NE(cursor_reason.raw(), normal_reason.raw());
}

TEST(GrblPanelJobGuardTest, StaleGeneratedEditorSendIsBlocked)
{
    GrblEditorSendGuardInput input;
    input.has_executable_content = true;
    input.stale_generated_gcode = true;

    auto const reason = get_grbl_editor_send_block_reason(input);
    EXPECT_EQ(reason.raw(), build_editor_gcode_stale_mapping_message().raw());
}

TEST(GrblPanelJobGuardTest, OutOfBedEditorSendUsesSharedBoundsMessage)
{
    GrblEditorSendGuardInput input;
    input.has_executable_content = true;
    input.check_bed_bounds = true;
    input.bounds.saw_xy_motion = true;
    input.bounds.min_x_mm = -1.0;
    input.bounds.max_x_mm = 20.0;
    input.bounds.min_y_mm = 0.0;
    input.bounds.max_y_mm = 10.0;
    input.bed_width_mm = 15.0;
    input.bed_height_mm = 15.0;

    auto const reason = get_grbl_editor_send_block_reason(input);
    EXPECT_EQ(reason.raw(), build_editor_gcode_out_of_bed_message(input.bounds, 15.0, 15.0).raw());
}

TEST(GrblPanelJobGuardTest, DirectSendConnectionStateHasSpecificMessages)
{
    GrblDirectSendGuardInput input;
    input.connection_state = GrblDirectSendConnectionState::ready_serial;
    EXPECT_TRUE(get_grbl_direct_send_block_reason(input).empty());

    input.connection_state = GrblDirectSendConnectionState::not_connected;
    auto const not_connected = get_grbl_direct_send_block_reason(input);

    input.connection_state = GrblDirectSendConnectionState::tcp_connected;
    auto const tcp_connected = get_grbl_direct_send_block_reason(input);
    EXPECT_FALSE(not_connected.empty());
    EXPECT_FALSE(tcp_connected.empty());
    EXPECT_NE(not_connected.raw(), tcp_connected.raw());
}

TEST(GrblPanelJobGuardTest, DirectSendManualChangePromptRequiresParentWindow)
{
    GrblDirectSendGuardInput input;
    input.requires_parent_window = true;
    input.has_parent_window = false;

    auto const reason = get_grbl_direct_send_block_reason(input);
    EXPECT_FALSE(reason.empty());
}
