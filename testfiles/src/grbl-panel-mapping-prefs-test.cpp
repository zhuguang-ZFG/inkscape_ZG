// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-mapping-prefs.h"

using Inkscape::UI::Dialog::GrblPanelMappingPrefs;
using Inkscape::UI::Dialog::apply_grbl_tool_change_mode_id;
using Inkscape::UI::Dialog::get_grbl_tool_change_mode_id;

TEST(GrblPanelMappingPrefsTest, ToolChangeModeIdMatchesStoredFlags)
{
    EXPECT_STREQ(get_grbl_tool_change_mode_id(false, false, false), "none");
    EXPECT_STREQ(get_grbl_tool_change_mode_id(true, true, false), "manual");
    EXPECT_STREQ(get_grbl_tool_change_mode_id(true, false, true), "m6");
}

TEST(GrblPanelMappingPrefsTest, ApplyingModeIdUpdatesFlagsConsistently)
{
    GrblPanelMappingPrefs prefs;
    prefs.tool_change_point = true;

    apply_grbl_tool_change_mode_id("manual", prefs);
    EXPECT_TRUE(prefs.auto_pause_between_layers);
    EXPECT_TRUE(prefs.manual_pen_change);
    EXPECT_FALSE(prefs.tool_change_m6);
    EXPECT_FALSE(prefs.tool_change_point);

    prefs.tool_change_point = true;
    apply_grbl_tool_change_mode_id("m6", prefs);
    EXPECT_TRUE(prefs.auto_pause_between_layers);
    EXPECT_FALSE(prefs.manual_pen_change);
    EXPECT_TRUE(prefs.tool_change_m6);
    EXPECT_TRUE(prefs.tool_change_point);

    prefs.tool_change_point = true;
    apply_grbl_tool_change_mode_id("none", prefs);
    EXPECT_FALSE(prefs.auto_pause_between_layers);
    EXPECT_FALSE(prefs.manual_pen_change);
    EXPECT_FALSE(prefs.tool_change_m6);
    EXPECT_FALSE(prefs.tool_change_point);
}
