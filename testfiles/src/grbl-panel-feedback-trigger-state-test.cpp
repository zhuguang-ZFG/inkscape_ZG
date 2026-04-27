// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-feedback-trigger-state.h"

using Inkscape::UI::Dialog::GrblPlotFeedbackTrigger;
using Inkscape::UI::Dialog::make_grbl_plot_feedback_trigger_plan;

TEST(GrblPanelFeedbackTriggerStateTest, SchedulesPreviewRefreshForCanvasAndDocumentEvents)
{
    for (auto const trigger : {
             GrblPlotFeedbackTrigger::document_content_changed,
             GrblPlotFeedbackTrigger::selection_changed,
             GrblPlotFeedbackTrigger::preview_visibility_changed,
             GrblPlotFeedbackTrigger::layout_changed,
             GrblPlotFeedbackTrigger::work_origin_changed,
         }) {
        auto const plan = make_grbl_plot_feedback_trigger_plan(trigger);
        EXPECT_TRUE(plan.channels.refresh_summaries);
        EXPECT_TRUE(plan.channels.refresh_overlay);
        EXPECT_FALSE(plan.refresh_immediately);
    }
}

TEST(GrblPanelFeedbackTriggerStateTest, MappingChangesCanRefreshImmediatelyWithoutPreview)
{
    auto const immediate = make_grbl_plot_feedback_trigger_plan(GrblPlotFeedbackTrigger::mapping_preferences_changed, true);
    EXPECT_TRUE(immediate.channels.refresh_summaries);
    EXPECT_TRUE(immediate.channels.refresh_overlay);
    EXPECT_TRUE(immediate.refresh_immediately);

    auto const summaries_only =
        make_grbl_plot_feedback_trigger_plan(GrblPlotFeedbackTrigger::mapping_preferences_changed, false);
    EXPECT_TRUE(summaries_only.channels.refresh_summaries);
    EXPECT_FALSE(summaries_only.channels.refresh_overlay);
    EXPECT_TRUE(summaries_only.refresh_immediately);
}
