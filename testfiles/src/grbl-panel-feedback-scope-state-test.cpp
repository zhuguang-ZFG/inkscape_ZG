// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-feedback-scope-state.h"

using Inkscape::UI::Dialog::GrblPlotFeedbackChannels;
using Inkscape::UI::Dialog::make_grbl_plot_feedback_channels;

TEST(GrblPanelFeedbackScopeStateTest, FullTriggersRefreshSummaryAndOverlay)
{
    auto const channels = make_grbl_plot_feedback_channels(true, true);
    EXPECT_TRUE(channels.refresh_summaries);
    EXPECT_TRUE(channels.refresh_overlay);
}

TEST(GrblPanelFeedbackScopeStateTest, SummaryOnlyTriggerSkipsOverlay)
{
    auto const channels = make_grbl_plot_feedback_channels(true, false);
    EXPECT_TRUE(channels.refresh_summaries);
    EXPECT_FALSE(channels.refresh_overlay);
}
