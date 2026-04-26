// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-feedback-presentation.h"

using Inkscape::UI::Dialog::build_grbl_preview_build_error_status;
using Inkscape::UI::Dialog::build_grbl_preview_status_note;
using Inkscape::UI::Dialog::make_grbl_preview_overlay_ui_plan;

TEST(GrblPanelFeedbackPresentationTest, PreviewBuildErrorFallsBackBySpace)
{
    EXPECT_FALSE(build_grbl_preview_build_error_status("", false).empty());
    EXPECT_FALSE(build_grbl_preview_build_error_status("", true).empty());
    EXPECT_NE(build_grbl_preview_build_error_status("", false).raw(),
              build_grbl_preview_build_error_status("", true).raw());
}

TEST(GrblPanelFeedbackPresentationTest, PreviewBuildErrorPrefersDetailedError)
{
    EXPECT_EQ(build_grbl_preview_build_error_status("boom", true).raw(), "boom");
}

TEST(GrblPanelFeedbackPresentationTest, PreviewStatusNoteOnlyAppearsWhenClippedOrTruncated)
{
    EXPECT_TRUE(build_grbl_preview_status_note(false, 10, 10, false).empty());
    EXPECT_FALSE(build_grbl_preview_status_note(false, 5, 10, false).empty());
    EXPECT_FALSE(build_grbl_preview_status_note(true, 10, 10, true).empty());
}

TEST(GrblPanelFeedbackPresentationTest, OverlayUiPlanOnlyPostsWhenMachinePreviewIsActive)
{
    auto const inactive = make_grbl_preview_overlay_ui_plan(false, "note");
    EXPECT_FALSE(inactive.build_machine_axis);
    EXPECT_FALSE(inactive.post_status);
    EXPECT_FALSE(inactive.request_canvas_redraw);

    auto const active = make_grbl_preview_overlay_ui_plan(true, "note");
    EXPECT_TRUE(active.build_machine_axis);
    EXPECT_TRUE(active.post_status);
    EXPECT_TRUE(active.request_canvas_redraw);
    EXPECT_EQ(active.status.raw(), "note");
}
