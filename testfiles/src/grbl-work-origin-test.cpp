// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-work-origin.h"

using Inkscape::UI::Dialog::GrblWorkOriginAction;
using Inkscape::UI::Dialog::build_grbl_work_origin_command;

TEST(GrblWorkOriginTest, SetOriginUsesG92AndPostsStatus)
{
    auto const cmd = build_grbl_work_origin_command(GrblWorkOriginAction::set_origin_xy);

    ASSERT_EQ(cmd.lines.size(), 2u);
    EXPECT_EQ(cmd.lines[0], "G21");
    EXPECT_EQ(cmd.lines[1], "G92 X0 Y0");
    EXPECT_TRUE(cmd.refresh_preview);
    EXPECT_FALSE(cmd.success_status.empty());
}

TEST(GrblWorkOriginTest, GoToWorkZeroUsesAbsoluteRapidAndPostsStatus)
{
    auto const cmd = build_grbl_work_origin_command(GrblWorkOriginAction::goto_work_zero_xy);

    ASSERT_EQ(cmd.lines.size(), 3u);
    EXPECT_EQ(cmd.lines[0], "G21");
    EXPECT_EQ(cmd.lines[1], "G90");
    EXPECT_EQ(cmd.lines[2], "G0 X0 Y0");
    EXPECT_TRUE(cmd.refresh_preview);
    EXPECT_FALSE(cmd.success_status.empty());
}

TEST(GrblWorkOriginTest, CancelReturnUsesSameMotionWithoutUiStatus)
{
    auto const cmd = build_grbl_work_origin_command(GrblWorkOriginAction::return_after_cancel);

    ASSERT_EQ(cmd.lines.size(), 3u);
    EXPECT_EQ(cmd.lines[2], "G0 X0 Y0");
    EXPECT_TRUE(cmd.refresh_preview);
    EXPECT_TRUE(cmd.success_status.empty());
}
