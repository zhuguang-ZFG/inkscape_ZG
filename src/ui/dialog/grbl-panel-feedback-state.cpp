// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-feedback-state.h"

namespace Inkscape::UI::Dialog {

GrblPlotFeedbackSchedulePlan make_grbl_plot_feedback_schedule_plan(bool const preview_already_requested,
                                                                  bool const blocked, bool const refresh_preview,
                                                                  bool const dispatch_pending,
                                                                  bool const timer_connected)
{
    GrblPlotFeedbackSchedulePlan plan;
    plan.preview_requested = preview_already_requested || refresh_preview;
    if (blocked) {
        return plan;
    }

    plan.clear_preview_overlay = refresh_preview;
    plan.queue_idle_refresh = !dispatch_pending && !timer_connected;
    return plan;
}

GrblPlotFeedbackRefreshPlan make_grbl_plot_feedback_refresh_plan(bool const preview_already_requested,
                                                                bool const blocked, bool const refresh_preview,
                                                                bool const timer_connected)
{
    GrblPlotFeedbackRefreshPlan plan;
    plan.preview_requested = preview_already_requested || refresh_preview;
    if (blocked) {
        return plan;
    }

    plan.start_timer = !timer_connected;
    return plan;
}

GrblPlotFeedbackTimerPlan make_grbl_plot_feedback_timer_plan(bool const preview_requested, bool const preview_enabled)
{
    GrblPlotFeedbackTimerPlan plan;
    plan.refresh_summaries = true;
    plan.preview_requested = false;
    plan.sync_preview_overlay = preview_requested && preview_enabled;
    return plan;
}

} // namespace Inkscape::UI::Dialog
