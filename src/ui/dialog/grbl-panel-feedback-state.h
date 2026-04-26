// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared plot-feedback scheduling helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_STATE_H

namespace Inkscape::UI::Dialog {

struct GrblPlotFeedbackSchedulePlan
{
    bool preview_requested = false;
    bool clear_preview_overlay = false;
    bool queue_idle_refresh = false;
};

struct GrblPlotFeedbackRefreshPlan
{
    bool preview_requested = false;
    bool start_timer = false;
};

struct GrblPlotFeedbackTimerPlan
{
    bool preview_requested = false;
    bool refresh_summaries = false;
    bool sync_preview_overlay = false;
};

GrblPlotFeedbackSchedulePlan make_grbl_plot_feedback_schedule_plan(bool preview_already_requested, bool blocked,
                                                                  bool refresh_preview, bool dispatch_pending,
                                                                  bool timer_connected);
GrblPlotFeedbackRefreshPlan make_grbl_plot_feedback_refresh_plan(bool preview_already_requested, bool blocked,
                                                                bool refresh_preview, bool timer_connected);
GrblPlotFeedbackTimerPlan make_grbl_plot_feedback_timer_plan(bool preview_requested, bool preview_enabled);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_STATE_H
