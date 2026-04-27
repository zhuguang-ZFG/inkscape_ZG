// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-feedback-state.h"

namespace Inkscape::UI::Dialog {

GrblPlotFeedbackSchedulePlan make_grbl_plot_feedback_schedule_plan(bool const summaries_already_requested,
                                                                  bool const overlay_already_requested,
                                                                  bool const blocked,
                                                                  bool const refresh_summaries,
                                                                  bool const refresh_overlay,
                                                                  bool const dispatch_pending,
                                                                  bool const timer_connected)
{
    GrblPlotFeedbackSchedulePlan plan;
    plan.refresh_summaries_requested = summaries_already_requested || refresh_summaries;
    plan.refresh_overlay_requested = overlay_already_requested || refresh_overlay;
    plan.clear_preview_overlay = refresh_overlay;
    if (blocked) {
        return plan;
    }

    plan.queue_idle_refresh = !dispatch_pending && !timer_connected;
    return plan;
}

GrblPlotFeedbackRefreshPlan make_grbl_plot_feedback_refresh_plan(bool const summaries_already_requested,
                                                                bool const overlay_already_requested,
                                                                bool const blocked,
                                                                bool const refresh_summaries,
                                                                bool const refresh_overlay,
                                                                bool const timer_connected)
{
    GrblPlotFeedbackRefreshPlan plan;
    plan.refresh_summaries_requested = summaries_already_requested || refresh_summaries;
    plan.refresh_overlay_requested = overlay_already_requested || refresh_overlay;
    if (blocked) {
        return plan;
    }

    plan.start_timer = !timer_connected;
    return plan;
}

GrblPlotFeedbackTimerPlan make_grbl_plot_feedback_timer_plan(bool const refresh_summaries_requested,
                                                             bool const refresh_overlay_requested,
                                                             bool const preview_enabled)
{
    GrblPlotFeedbackTimerPlan plan;
    plan.refresh_summaries = refresh_summaries_requested;
    plan.refresh_summaries_requested = false;
    plan.refresh_overlay_requested = false;
    plan.sync_preview_overlay = refresh_overlay_requested && preview_enabled;
    return plan;
}

} // namespace Inkscape::UI::Dialog
