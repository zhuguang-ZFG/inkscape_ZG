// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-editor-gcode.h"

using Inkscape::UI::Dialog::EditorGcodeBounds;
using Inkscape::UI::Dialog::analyze_editor_gcode_bounds_mm;
using Inkscape::UI::Dialog::build_editor_gcode_out_of_bed_message;
using Inkscape::UI::Dialog::count_executable_editor_gcode_lines;
using Inkscape::UI::Dialog::nearly_equal_mm;

TEST(GrblEditorGcodeTest, TracksAbsoluteAndRelativeMovesInMillimeters)
{
    EditorGcodeBounds bounds;
    ASSERT_TRUE(analyze_editor_gcode_bounds_mm("G21\nG90\nG0 X10 Y20\nG91\nG1 X5 Y-2\n", bounds));

    EXPECT_TRUE(bounds.saw_xy_motion);
    EXPECT_DOUBLE_EQ(bounds.min_x_mm, 10.0);
    EXPECT_DOUBLE_EQ(bounds.max_x_mm, 15.0);
    EXPECT_DOUBLE_EQ(bounds.min_y_mm, 18.0);
    EXPECT_DOUBLE_EQ(bounds.max_y_mm, 20.0);
}

TEST(GrblEditorGcodeTest, TracksInchModeAndSkipsComments)
{
    EditorGcodeBounds bounds;
    ASSERT_TRUE(analyze_editor_gcode_bounds_mm(
        "; comment\n"
        "(comment)\n"
        "G20\n"
        "G90\n"
        "G0 X1.0 Y0.5\n"
        "M3 S1000\n",
        bounds));

    EXPECT_TRUE(bounds.saw_xy_motion);
    EXPECT_NEAR(bounds.min_x_mm, 25.4, 1e-9);
    EXPECT_NEAR(bounds.max_x_mm, 25.4, 1e-9);
    EXPECT_NEAR(bounds.min_y_mm, 12.7, 1e-9);
    EXPECT_NEAR(bounds.max_y_mm, 12.7, 1e-9);
}

TEST(GrblEditorGcodeTest, BuildsOutOfBedMessageWithBounds)
{
    EditorGcodeBounds bounds;
    bounds.saw_xy_motion = true;
    bounds.min_x_mm = -1.0;
    bounds.max_x_mm = 210.0;
    bounds.min_y_mm = 0.0;
    bounds.max_y_mm = 105.5;

    auto const message = build_editor_gcode_out_of_bed_message(bounds, 200.0, 100.0).raw();

    EXPECT_NE(message.find("X -1.000..210.000 mm"), std::string::npos);
    EXPECT_NE(message.find("Y 0.000..105.500 mm"), std::string::npos);
    EXPECT_NE(message.find("X 0..200.000 mm"), std::string::npos);
    EXPECT_NE(message.find("Y 0..100.000 mm"), std::string::npos);
}

TEST(GrblEditorGcodeTest, NearlyEqualUsesTolerance)
{
    EXPECT_TRUE(nearly_equal_mm(10.0, 10.0 + 5e-7));
    EXPECT_FALSE(nearly_equal_mm(10.0, 10.0 + 5e-4));
}

TEST(GrblEditorGcodeTest, ExecutableLineCountSkipsCommentsAndCanClamp)
{
    auto const text =
        "; comment\n"
        "(comment)\n"
        " \n"
        "G0 X1\n"
        "G1 Y2\n";

    EXPECT_EQ(count_executable_editor_gcode_lines(text, 10), 2u);
    EXPECT_EQ(count_executable_editor_gcode_lines(text, 1), 2u);
}
