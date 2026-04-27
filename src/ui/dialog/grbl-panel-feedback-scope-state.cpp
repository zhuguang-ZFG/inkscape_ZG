// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-feedback-scope-state.h"

namespace Inkscape::UI::Dialog {

GrblPlotFeedbackChannels make_grbl_plot_feedback_channels(bool const refresh_summaries, bool const refresh_overlay)
{
    return {
        .refresh_summaries = refresh_summaries,
        .refresh_overlay = refresh_overlay,
    };
}

} // namespace Inkscape::UI::Dialog
