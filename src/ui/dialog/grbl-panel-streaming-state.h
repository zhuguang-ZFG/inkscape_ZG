// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Testable GRBL streaming state transitions.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_STATE_H

#include <cstddef>
#include <string>

#include "grbl-panel-streaming-reply.h"

namespace Inkscape::UI::Dialog {

enum class GrblStreamingPhase {
    idle,
    running,
    paused,
    cancelling,
    complete,
    error,
    alarm,
};

class GrblPanelStreamingState
{
public:
    void start(std::size_t total_lines);
    void line_written();
    void apply_reply(GrblStreamingReply const &reply);
    void fail(std::string error_text);
    void request_pause();
    bool resume();
    bool request_cancel();
    void finish_if_complete();
    void disconnect();

    GrblStreamingPhase phase() const { return _phase; }
    std::size_t total_lines() const { return _total_lines; }
    std::size_t lines_written() const { return _lines_written; }
    std::size_t lines_acknowledged() const { return _lines_acknowledged; }
    std::size_t in_flight_lines() const;
    std::string const &error_text() const { return _error_text; }
    std::string const &alarm_text() const { return _alarm_text; }
    std::string const &runtime_state() const { return _runtime_state; }

    bool can_feed_more_lines() const;
    bool can_resume() const;
    bool is_terminal() const;

private:
    GrblStreamingPhase _phase = GrblStreamingPhase::idle;
    std::size_t _total_lines = 0;
    std::size_t _lines_written = 0;
    std::size_t _lines_acknowledged = 0;
    std::string _error_text;
    std::string _alarm_text;
    std::string _runtime_state;
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_STATE_H
