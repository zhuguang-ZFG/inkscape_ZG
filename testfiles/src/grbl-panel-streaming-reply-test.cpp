// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-streaming-reply.h"

using Inkscape::UI::Dialog::GrblStreamingReplyKind;
using Inkscape::UI::Dialog::parse_grbl_streaming_reply;

TEST(GrblPanelStreamingReplyTest, ClassifiesOk)
{
    auto const reply = parse_grbl_streaming_reply("ok");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::ok);
    EXPECT_EQ(reply.code, 0);
    EXPECT_EQ(reply.text, "ok");
}

TEST(GrblPanelStreamingReplyTest, ClassifiesErrorWithCode)
{
    auto const reply = parse_grbl_streaming_reply("error:33");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::error);
    EXPECT_EQ(reply.code, 33);
    EXPECT_EQ(reply.text, "error:33");
}

TEST(GrblPanelStreamingReplyTest, ClassifiesAlarmWithCode)
{
    auto const reply = parse_grbl_streaming_reply("ALARM:2");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::alarm);
    EXPECT_EQ(reply.code, 2);
    EXPECT_EQ(reply.text, "ALARM:2");
}

TEST(GrblPanelStreamingReplyTest, ClassifiesRuntimeStatus)
{
    auto const reply = parse_grbl_streaming_reply("<Run|MPos:1.000,2.000,0.000|FS:1200,0>");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::runtime_status);
    EXPECT_EQ(reply.runtime_state, "Run");
}

TEST(GrblPanelStreamingReplyTest, ClassifiesInfoAndUnknown)
{
    EXPECT_EQ(parse_grbl_streaming_reply("Grbl 1.1h ['$' for help]").kind,
              GrblStreamingReplyKind::info);
    EXPECT_EQ(parse_grbl_streaming_reply("[MSG:Reset to continue]").kind,
              GrblStreamingReplyKind::info);
    EXPECT_EQ(parse_grbl_streaming_reply("unexpected text").kind,
              GrblStreamingReplyKind::unknown);
}

TEST(GrblPanelStreamingReplyTest, TrimsWhitespace)
{
    auto const reply = parse_grbl_streaming_reply("  ok\r");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::ok);
    EXPECT_EQ(reply.text, "ok");
}
