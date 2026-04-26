// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-feedback-state.h"

using Inkscape::UI::Dialog::make_grbl_plot_feedback_refresh_plan;
using Inkscape::UI::Dialog::make_grbl_plot_feedback_schedule_plan;
using Inkscape::UI::Dialog::make_grbl_plot_feedback_timer_plan;

TEST(GrblPanelFeedbackStateTest, SchedulePlanQueuesIdleRefreshOnlyWhenUnblockedAndIdle)
{
    auto const plan = make_grbl_plot_feedback_schedule_plan(false, false, true, false, false);
    EXPECT_TRUE(plan.preview_requested);
    EXPECT_TRUE(plan.clear_preview_overlay);
    EXPECT_TRUE(plan.queue_idle_refresh);

    auto const blocked = make_grbl_plot_feedback_schedule_plan(false, true, true, false, false);
    EXPECT_TRUE(blocked.preview_requested);
    EXPECT_FALSE(blocked.clear_preview_overlay);
    EXPECT_FALSE(blocked.queue_idle_refresh);
}

TEST(GrblPanelFeedbackStateTest, RefreshPlanStartsTimerOnlyWhenNeeded)
{
    auto const start = make_grbl_plot_feedback_refresh_plan(false, false, true, false);
    EXPECT_TRUE(start.preview_requested);
    EXPECT_TRUE(start.start_timer);

    auto const already_running = make_grbl_plot_feedback_refresh_plan(true, false, false, true);
    EXPECT_TRUE(already_running.preview_requested);
    EXPECT_FALSE(already_running.start_timer);
}

TEST(GrblPanelFeedbackStateTest, TimerPlanResetsPreviewLatchAndHonorsPreviewToggle)
{
    auto const enabled = make_grbl_plot_feedback_timer_plan(true, true);
    EXPECT_TRUE(enabled.refresh_summaries);
    EXPECT_FALSE(enabled.preview_requested);
    EXPECT_TRUE(enabled.sync_preview_overlay);

    auto const disabled = make_grbl_plot_feedback_timer_plan(true, false);
    EXPECT_TRUE(disabled.refresh_summaries);
    EXPECT_FALSE(disabled.preview_requested);
    EXPECT_FALSE(disabled.sync_preview_overlay);
}
