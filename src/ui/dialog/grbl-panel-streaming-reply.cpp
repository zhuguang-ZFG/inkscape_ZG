// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-streaming-reply.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace Inkscape::UI::Dialog {
namespace {

std::string trim_ascii(std::string text)
{
    auto const is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [&](char ch) {
                   return !is_space(static_cast<unsigned char>(ch));
               }));
    text.erase(std::find_if(text.rbegin(), text.rend(), [&](char ch) {
                   return !is_space(static_cast<unsigned char>(ch));
               }).base(),
               text.end());
    return text;
}

bool starts_with_ci(std::string const &text, char const *prefix)
{
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= text.size()) {
            return false;
        }

        auto const lhs = static_cast<unsigned char>(text[i]);
        auto const rhs = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

int parse_code_after_colon(std::string const &text)
{
    auto const colon = text.find(':');
    if (colon == std::string::npos || colon + 1 >= text.size()) {
        return 0;
    }

    return std::atoi(text.c_str() + colon + 1);
}

} // namespace

GrblStreamingReply parse_grbl_streaming_reply(std::string const &line)
{
    GrblStreamingReply reply;
    reply.text = trim_ascii(line);

    if (reply.text == "ok") {
        reply.kind = GrblStreamingReplyKind::ok;
        return reply;
    }

    if (starts_with_ci(reply.text, "error:")) {
        reply.kind = GrblStreamingReplyKind::error;
        reply.code = parse_code_after_colon(reply.text);
        return reply;
    }

    if (starts_with_ci(reply.text, "ALARM:")) {
        reply.kind = GrblStreamingReplyKind::alarm;
        reply.code = parse_code_after_colon(reply.text);
        return reply;
    }

    if (reply.text.size() >= 3 && reply.text.front() == '<') {
        auto const bar = reply.text.find('|');
        auto const end = reply.text.find('>');
        auto const state_end = bar == std::string::npos ? end : bar;
        if (state_end != std::string::npos && state_end > 1) {
            reply.kind = GrblStreamingReplyKind::runtime_status;
            reply.runtime_state = reply.text.substr(1, state_end - 1);
            return reply;
        }
    }

    if (starts_with_ci(reply.text, "Grbl ") || starts_with_ci(reply.text, "[MSG:") ||
        starts_with_ci(reply.text, "[GC:") || starts_with_ci(reply.text, "[VER:") ||
        starts_with_ci(reply.text, "[OPT:") || starts_with_ci(reply.text, "$")) {
        reply.kind = GrblStreamingReplyKind::info;
        return reply;
    }

    return reply;
}
