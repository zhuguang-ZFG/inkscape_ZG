// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-pen-command.h"

using Inkscape::UI::Dialog::GrblPenMotion;
using Inkscape::UI::Dialog::build_grbl_pen_command;

TEST(GrblPenCommandTest, BuildsM3M5CommandsForPenUpAndDown)
{
    auto const up = build_grbl_pen_command("m3m5", GrblPenMotion::up, "ignored");
    ASSERT_TRUE(up.ok);
    ASSERT_EQ(up.lines.size(), 1u);
    EXPECT_EQ(up.lines[0], "M5");

    auto const down = build_grbl_pen_command("M3M5", GrblPenMotion::down, "ignored");
    ASSERT_TRUE(down.ok);
    ASSERT_EQ(down.lines.size(), 1u);
    EXPECT_EQ(down.lines[0], "M3 S1000");
}

TEST(GrblPenCommandTest, SplitsAndNormalizesConfiguredZCommands)
{
    auto const cmd = build_grbl_pen_command(
        "z", GrblPenMotion::up,
        " \n; comment\nG90\r\n G1 Z0 F1200 \n(comment)\n");

    ASSERT_TRUE(cmd.ok);
    ASSERT_EQ(cmd.lines.size(), 2u);
    EXPECT_EQ(cmd.lines[0], "G90");
    EXPECT_EQ(cmd.lines[1], "G1 Z0 F1200");
}

TEST(GrblPenCommandTest, ReportsEmptyConfiguredCommandAfterFiltering)
{
    auto const up = build_grbl_pen_command("z", GrblPenMotion::up, " \n(comment)\n; comment\n");
    EXPECT_FALSE(up.ok);
    EXPECT_TRUE(up.lines.empty());
    EXPECT_FALSE(up.error.empty());

    auto const down = build_grbl_pen_command("z", GrblPenMotion::down, "");
    EXPECT_FALSE(down.ok);
    EXPECT_TRUE(down.lines.empty());
    EXPECT_FALSE(down.error.empty());
}
