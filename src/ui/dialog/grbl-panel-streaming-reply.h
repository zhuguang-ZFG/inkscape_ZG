// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * GRBL streaming reply parsing helpers.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_REPLY_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_REPLY_H

#include <string>

namespace Inkscape::UI::Dialog {

enum class GrblStreamingReplyKind {
    ok,
    error,
    alarm,
    runtime_status,
    info,
    unknown,
};

struct GrblStreamingReply
{
    GrblStreamingReplyKind kind = GrblStreamingReplyKind::unknown;
    int code = 0;
    std::string runtime_state;
    std::string text;
};

GrblStreamingReply parse_grbl_streaming_reply(std::string const &line);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_REPLY_H
