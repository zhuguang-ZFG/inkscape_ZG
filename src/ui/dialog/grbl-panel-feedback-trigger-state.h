// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared semantic trigger mapping for GRBL plot-feedback refreshes.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_TRIGGER_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_TRIGGER_STATE_H

namespace Inkscape::UI::Dialog {

enum class GrblPlotFeedbackTrigger {
    document_content_changed,
    selection_changed,
    preview_visibility_changed,
    layout_changed,
    work_origin_changed,
    mapping_preferences_changed,
};

struct GrblPlotFeedbackTriggerPlan
{
    bool refresh_preview = false;
    bool refresh_immediately = false;
};

GrblPlotFeedbackTriggerPlan make_grbl_plot_feedback_trigger_plan(GrblPlotFeedbackTrigger trigger,
                                                                 bool refresh_preview = true);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_TRIGGER_STATE_H
