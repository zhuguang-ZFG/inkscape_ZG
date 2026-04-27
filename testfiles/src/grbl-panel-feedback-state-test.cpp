// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-feedback-state.h"

using Inkscape::UI::Dialog::make_grbl_plot_feedback_refresh_plan;
using Inkscape::UI::Dialog::make_grbl_plot_feedback_schedule_plan;
using Inkscape::UI::Dialog::make_grbl_plot_feedback_timer_plan;

TEST(GrblPanelFeedbackStateTest, SchedulePlanQueuesIdleRefreshOnlyWhenUnblockedAndIdle)
{
    auto const plan = make_grbl_plot_feedback_schedule_plan(false, false, false, true, true, false, false);
    EXPECT_TRUE(plan.refresh_summaries_requested);
    EXPECT_TRUE(plan.refresh_overlay_requested);
    EXPECT_TRUE(plan.clear_preview_overlay);
    EXPECT_TRUE(plan.queue_idle_refresh);

    auto const blocked = make_grbl_plot_feedback_schedule_plan(false, false, true, true, true, false, false);
    EXPECT_TRUE(blocked.refresh_summaries_requested);
    EXPECT_TRUE(blocked.refresh_overlay_requested);
    EXPECT_TRUE(blocked.clear_preview_overlay);
    EXPECT_FALSE(blocked.queue_idle_refresh);
}

TEST(GrblPanelFeedbackStateTest, RefreshPlanStartsTimerOnlyWhenNeeded)
{
    auto const start = make_grbl_plot_feedback_refresh_plan(false, false, false, true, true, false);
    EXPECT_TRUE(start.refresh_summaries_requested);
    EXPECT_TRUE(start.refresh_overlay_requested);
    EXPECT_TRUE(start.start_timer);

    auto const already_running = make_grbl_plot_feedback_refresh_plan(true, true, false, false, false, true);
    EXPECT_TRUE(already_running.refresh_summaries_requested);
    EXPECT_TRUE(already_running.refresh_overlay_requested);
    EXPECT_FALSE(already_running.start_timer);
}

TEST(GrblPanelFeedbackStateTest, TimerPlanResetsPreviewLatchAndHonorsPreviewToggle)
{
    auto const enabled = make_grbl_plot_feedback_timer_plan(true, true, true);
    EXPECT_TRUE(enabled.refresh_summaries);
    EXPECT_FALSE(enabled.refresh_summaries_requested);
    EXPECT_FALSE(enabled.refresh_overlay_requested);
    EXPECT_TRUE(enabled.sync_preview_overlay);

    auto const disabled = make_grbl_plot_feedback_timer_plan(true, true, false);
    EXPECT_TRUE(disabled.refresh_summaries);
    EXPECT_FALSE(disabled.refresh_summaries_requested);
    EXPECT_FALSE(disabled.refresh_overlay_requested);
    EXPECT_FALSE(disabled.sync_preview_overlay);
}
