// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared two-channel refresh scope helpers for GRBL plot feedback.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_SCOPE_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_SCOPE_STATE_H

namespace Inkscape::UI::Dialog {

struct GrblPlotFeedbackChannels
{
    bool refresh_summaries = false;
    bool refresh_overlay = false;
};

GrblPlotFeedbackChannels make_grbl_plot_feedback_channels(bool refresh_summaries, bool refresh_overlay);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_SCOPE_STATE_H
