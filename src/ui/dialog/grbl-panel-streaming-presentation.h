// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * UI presentation helpers for GRBL streaming state.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_PRESENTATION_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_PRESENTATION_H

#include <string>

#include <glibmm/ustring.h>

#include "grbl-panel-streaming-state.h"

namespace Inkscape::UI::Dialog {

struct GrblStreamingPresentation
{
    bool visible = false;
    double progress_fraction = 0.0;
    Glib::ustring progress_label;
    Glib::ustring in_flight_label;
    Glib::ustring phase_label;
    Glib::ustring blocking_label;
    std::string css_class;
    bool pause_sensitive = false;
    bool resume_sensitive = false;
    bool stop_sensitive = false;
};

GrblStreamingPresentation make_grbl_streaming_presentation(GrblPanelStreamingState const &state);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_PRESENTATION_H
