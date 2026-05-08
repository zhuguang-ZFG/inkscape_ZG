// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-streaming-presentation.h"

#include <algorithm>

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {
namespace {

Glib::ustring phase_label_for(GrblStreamingPhase const phase)
{
    switch (phase) {
        case GrblStreamingPhase::running:
            return _("Running");
        case GrblStreamingPhase::paused:
            return _("Paused");
        case GrblStreamingPhase::cancelling:
            return _("Cancelling");
        case GrblStreamingPhase::complete:
            return _("Complete");
        case GrblStreamingPhase::error:
            return _("Error");
        case GrblStreamingPhase::alarm:
            return _("Alarm");
        case GrblStreamingPhase::idle:
        default:
            return _("Idle");
    }
}

std::string css_class_for(GrblStreamingPhase const phase)
{
    switch (phase) {
        case GrblStreamingPhase::paused:
        case GrblStreamingPhase::cancelling:
            return "warning";
        case GrblStreamingPhase::error:
        case GrblStreamingPhase::alarm:
            return "error";
        case GrblStreamingPhase::complete:
            return "success";
        case GrblStreamingPhase::running:
            return "running";
        case GrblStreamingPhase::idle:
        default:
            return "idle";
    }
}

} // namespace

GrblStreamingPresentation make_grbl_streaming_presentation(GrblPanelStreamingState const &state)
{
    GrblStreamingPresentation presentation;
    presentation.visible = state.phase() != GrblStreamingPhase::idle;
    presentation.phase_label = phase_label_for(state.phase());
    presentation.css_class = css_class_for(state.phase());

    auto const total = state.total_lines();
    auto const acknowledged = state.lines_acknowledged();
    if (total > 0) {
        presentation.progress_fraction = std::min(1.0, static_cast<double>(acknowledged) / total);
        presentation.progress_label = Glib::ustring::compose(_("Sent %1 / %2 lines"),
                                                             static_cast<guint64>(acknowledged),
                                                             static_cast<guint64>(total));
    } else {
        presentation.progress_fraction = 0.0;
        presentation.progress_label = _("No active stream");
    }

    presentation.in_flight_label = Glib::ustring::compose(_("Awaiting %1 acknowledgements"),
                                                          static_cast<guint64>(state.in_flight_lines()));

    if (state.phase() == GrblStreamingPhase::error) {
        presentation.blocking_label =
            Glib::ustring::compose(_("Streaming stopped on %1"), state.error_text());
    } else if (state.phase() == GrblStreamingPhase::alarm) {
        presentation.blocking_label =
            Glib::ustring::compose(_("Controller alarm: %1"), state.alarm_text());
    } else if (!state.runtime_state().empty()) {
        presentation.blocking_label =
            Glib::ustring::compose(_("Controller state: %1"), state.runtime_state());
    }

    presentation.pause_sensitive = state.phase() == GrblStreamingPhase::running;
    presentation.resume_sensitive = state.can_resume();
    presentation.stop_sensitive = state.phase() == GrblStreamingPhase::running ||
                                  state.phase() == GrblStreamingPhase::paused;
    return presentation;
}

} // namespace Inkscape::UI::Dialog
