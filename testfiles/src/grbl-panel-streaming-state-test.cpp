// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-streaming-reply.h"
#include "src/ui/dialog/grbl-panel-streaming-state.h"

using Inkscape::UI::Dialog::GrblPanelStreamingState;
using Inkscape::UI::Dialog::GrblStreamingPhase;
using Inkscape::UI::Dialog::parse_grbl_streaming_reply;

TEST(GrblPanelStreamingStateTest, StartInitializesTotals)
{
    GrblPanelStreamingState state;
    state.start(10);

    EXPECT_EQ(state.phase(), GrblStreamingPhase::running);
    EXPECT_EQ(state.total_lines(), 10u);
    EXPECT_EQ(state.lines_written(), 0u);
    EXPECT_EQ(state.lines_acknowledged(), 0u);
    EXPECT_EQ(state.in_flight_lines(), 0u);
}

TEST(GrblPanelStreamingStateTest, OkAdvancesAcknowledgements)
{
    GrblPanelStreamingState state;
    state.start(3);
    state.line_written();
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("ok"));

    EXPECT_EQ(state.lines_written(), 2u);
    EXPECT_EQ(state.lines_acknowledged(), 1u);
    EXPECT_EQ(state.in_flight_lines(), 1u);
    EXPECT_FALSE(state.is_terminal());
}

TEST(GrblPanelStreamingStateTest, CompleteWhenAllWrittenLinesAreAcknowledged)
{
    GrblPanelStreamingState state;
    state.start(2);
    state.line_written();
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("ok"));
    state.apply_reply(parse_grbl_streaming_reply("ok"));
    state.finish_if_complete();

    EXPECT_EQ(state.phase(), GrblStreamingPhase::complete);
    EXPECT_TRUE(state.is_terminal());
}

TEST(GrblPanelStreamingStateTest, ErrorBlocksFurtherSending)
{
    GrblPanelStreamingState state;
    state.start(2);
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("error:33"));

    EXPECT_EQ(state.phase(), GrblStreamingPhase::error);
    EXPECT_TRUE(state.is_terminal());
    EXPECT_FALSE(state.can_feed_more_lines());
    EXPECT_EQ(state.error_text(), "error:33");
}

TEST(GrblPanelStreamingStateTest, ExplicitFailureBlocksFurtherSending)
{
    GrblPanelStreamingState state;
    state.start(2);
    state.fail("serial write failed");

    EXPECT_EQ(state.phase(), GrblStreamingPhase::error);
    EXPECT_TRUE(state.is_terminal());
    EXPECT_FALSE(state.can_feed_more_lines());
    EXPECT_EQ(state.error_text(), "serial write failed");
}

TEST(GrblPanelStreamingStateTest, AlarmBlocksResume)
{
    GrblPanelStreamingState state;
    state.start(2);
    state.request_pause();
    state.apply_reply(parse_grbl_streaming_reply("ALARM:2"));

    EXPECT_EQ(state.phase(), GrblStreamingPhase::alarm);
    EXPECT_TRUE(state.is_terminal());
    EXPECT_FALSE(state.can_resume());
    EXPECT_EQ(state.alarm_text(), "ALARM:2");
}

TEST(GrblPanelStreamingStateTest, PauseAndResumeAreExplicit)
{
    GrblPanelStreamingState state;
    state.start(4);
    state.line_written();
    state.request_pause();

    EXPECT_EQ(state.phase(), GrblStreamingPhase::paused);
    EXPECT_FALSE(state.can_feed_more_lines());
    EXPECT_TRUE(state.can_resume());
    EXPECT_EQ(state.in_flight_lines(), 1u);

    state.resume();
    EXPECT_EQ(state.phase(), GrblStreamingPhase::running);
    EXPECT_TRUE(state.can_feed_more_lines());
}

TEST(GrblPanelStreamingStateTest, CancelIsIdempotent)
{
    GrblPanelStreamingState state;
    state.start(4);
    EXPECT_TRUE(state.request_cancel());
    EXPECT_FALSE(state.request_cancel());
    EXPECT_EQ(state.phase(), GrblStreamingPhase::cancelling);
    EXPECT_FALSE(state.can_feed_more_lines());
}

TEST(GrblPanelStreamingStateTest, DisconnectResetsToIdle)
{
    GrblPanelStreamingState state;
    state.start(4);
    state.line_written();
    state.disconnect();

    EXPECT_EQ(state.phase(), GrblStreamingPhase::idle);
    EXPECT_EQ(state.total_lines(), 0u);
    EXPECT_EQ(state.in_flight_lines(), 0u);
}
