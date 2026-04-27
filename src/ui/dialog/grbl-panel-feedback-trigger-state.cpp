// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-feedback-trigger-state.h"

namespace Inkscape::UI::Dialog {

GrblPlotFeedbackTriggerPlan make_grbl_plot_feedback_trigger_plan(GrblPlotFeedbackTrigger const trigger,
                                                                 bool const refresh_preview)
{
    GrblPlotFeedbackTriggerPlan plan;
    switch (trigger) {
        case GrblPlotFeedbackTrigger::document_content_changed:
        case GrblPlotFeedbackTrigger::selection_changed:
        case GrblPlotFeedbackTrigger::preview_visibility_changed:
        case GrblPlotFeedbackTrigger::layout_changed:
        case GrblPlotFeedbackTrigger::work_origin_changed:
            plan.channels = make_grbl_plot_feedback_channels(true, true);
            break;
        case GrblPlotFeedbackTrigger::mapping_preferences_changed:
            plan.channels = make_grbl_plot_feedback_channels(true, refresh_preview);
            plan.refresh_immediately = true;
            break;
    }
    return plan;
}

} // namespace Inkscape::UI::Dialog
