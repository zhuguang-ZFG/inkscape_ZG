// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-streaming-state.h"

#include <algorithm>
#include <utility>

namespace Inkscape::UI::Dialog {

void GrblPanelStreamingState::start(std::size_t const total_lines)
{
    _phase = GrblStreamingPhase::running;
    _total_lines = total_lines;
    _lines_written = 0;
    _lines_acknowledged = 0;
    _error_text.clear();
    _alarm_text.clear();
    _runtime_state.clear();
}

void GrblPanelStreamingState::line_written()
{
    if (!can_feed_more_lines()) {
        return;
    }

    ++_lines_written;
}

void GrblPanelStreamingState::apply_reply(GrblStreamingReply const &reply)
{
    switch (reply.kind) {
        case GrblStreamingReplyKind::ok:
            if (_lines_acknowledged < _lines_written) {
                ++_lines_acknowledged;
            }
            break;
        case GrblStreamingReplyKind::error:
            fail(reply.text);
            break;
        case GrblStreamingReplyKind::alarm:
            _phase = GrblStreamingPhase::alarm;
            _alarm_text = reply.text;
            break;
        case GrblStreamingReplyKind::runtime_status:
            _runtime_state = reply.runtime_state;
            break;
        case GrblStreamingReplyKind::info:
        case GrblStreamingReplyKind::unknown:
            break;
    }
}

void GrblPanelStreamingState::fail(std::string error_text)
{
    _phase = GrblStreamingPhase::error;
    _error_text = std::move(error_text);
}

void GrblPanelStreamingState::request_pause()
{
    if (_phase == GrblStreamingPhase::running) {
        _phase = GrblStreamingPhase::paused;
    }
}

bool GrblPanelStreamingState::resume()
{
    if (!can_resume()) {
        return false;
    }

    _phase = GrblStreamingPhase::running;
    return true;
}

bool GrblPanelStreamingState::request_cancel()
{
    if (_phase != GrblStreamingPhase::running && _phase != GrblStreamingPhase::paused) {
        return false;
    }

    _phase = GrblStreamingPhase::cancelling;
    return true;
}

void GrblPanelStreamingState::finish_if_complete()
{
    if (_phase != GrblStreamingPhase::running && _phase != GrblStreamingPhase::paused &&
        _phase != GrblStreamingPhase::cancelling) {
        return;
    }

    if (_lines_written >= _total_lines && _lines_acknowledged >= _total_lines) {
        _phase = GrblStreamingPhase::complete;
    }
}

void GrblPanelStreamingState::disconnect()
{
    _phase = GrblStreamingPhase::idle;
    _total_lines = 0;
    _lines_written = 0;
    _lines_acknowledged = 0;
    _error_text.clear();
    _alarm_text.clear();
    _runtime_state.clear();
}

std::size_t GrblPanelStreamingState::in_flight_lines() const
{
    return _lines_written - std::min(_lines_written, _lines_acknowledged);
}

bool GrblPanelStreamingState::can_feed_more_lines() const
{
    return _phase == GrblStreamingPhase::running && _lines_written < _total_lines;
}

bool GrblPanelStreamingState::can_resume() const
{
    return _phase == GrblStreamingPhase::paused;
}

bool GrblPanelStreamingState::is_terminal() const
{
    return _phase == GrblStreamingPhase::complete || _phase == GrblStreamingPhase::error ||
           _phase == GrblStreamingPhase::alarm;
}

} // namespace Inkscape::UI::Dialog
