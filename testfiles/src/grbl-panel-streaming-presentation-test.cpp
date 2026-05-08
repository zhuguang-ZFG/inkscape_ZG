// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-streaming-presentation.h"
#include "src/ui/dialog/grbl-panel-streaming-state.h"

using Inkscape::UI::Dialog::GrblPanelStreamingState;
using Inkscape::UI::Dialog::make_grbl_streaming_presentation;
using Inkscape::UI::Dialog::parse_grbl_streaming_reply;

TEST(GrblPanelStreamingPresentationTest, IdleHasNeutralCopy)
{
    GrblPanelStreamingState state;
    auto const presentation = make_grbl_streaming_presentation(state);

    EXPECT_FALSE(presentation.visible);
    EXPECT_EQ(presentation.progress_fraction, 0.0);
    EXPECT_FALSE(presentation.pause_sensitive);
    EXPECT_FALSE(presentation.resume_sensitive);
    EXPECT_FALSE(presentation.stop_sensitive);
}

TEST(GrblPanelStreamingPresentationTest, RunningShowsProgressAndInFlight)
{
    GrblPanelStreamingState state;
    state.start(10);
    state.line_written();
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("ok"));

    auto const presentation = make_grbl_streaming_presentation(state);
    EXPECT_TRUE(presentation.visible);
    EXPECT_NE(presentation.progress_label.raw().find("1 / 10"), std::string::npos);
    EXPECT_NE(presentation.in_flight_label.raw().find("1"), std::string::npos);
    EXPECT_NEAR(presentation.progress_fraction, 0.1, 0.0001);
    EXPECT_TRUE(presentation.pause_sensitive);
    EXPECT_FALSE(presentation.resume_sensitive);
    EXPECT_TRUE(presentation.stop_sensitive);
}

TEST(GrblPanelStreamingPresentationTest, PausedEnablesResume)
{
    GrblPanelStreamingState state;
    state.start(10);
    state.request_pause();

    auto const presentation = make_grbl_streaming_presentation(state);
    EXPECT_NE(presentation.phase_label.raw().find("Paused"), std::string::npos);
    EXPECT_FALSE(presentation.pause_sensitive);
    EXPECT_TRUE(presentation.resume_sensitive);
    EXPECT_TRUE(presentation.stop_sensitive);
    EXPECT_EQ(presentation.css_class, "warning");
}

TEST(GrblPanelStreamingPresentationTest, ErrorAndAlarmAreBlocking)
{
    GrblPanelStreamingState error_state;
    error_state.start(10);
    error_state.apply_reply(parse_grbl_streaming_reply("error:33"));
    auto const error_presentation = make_grbl_streaming_presentation(error_state);
    EXPECT_NE(error_presentation.blocking_label.raw().find("error:33"), std::string::npos);
    EXPECT_EQ(error_presentation.css_class, "error");
    EXPECT_FALSE(error_presentation.resume_sensitive);

    GrblPanelStreamingState alarm_state;
    alarm_state.start(10);
    alarm_state.apply_reply(parse_grbl_streaming_reply("ALARM:2"));
    auto const alarm_presentation = make_grbl_streaming_presentation(alarm_state);
    EXPECT_NE(alarm_presentation.blocking_label.raw().find("ALARM:2"), std::string::npos);
    EXPECT_EQ(alarm_presentation.css_class, "error");
    EXPECT_FALSE(alarm_presentation.resume_sensitive);
}

TEST(GrblPanelStreamingPresentationTest, CompleteShowsDone)
{
    GrblPanelStreamingState state;
    state.start(1);
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("ok"));
    state.finish_if_complete();

    auto const presentation = make_grbl_streaming_presentation(state);
    EXPECT_TRUE(presentation.visible);
    EXPECT_NE(presentation.phase_label.raw().find("Complete"), std::string::npos);
    EXPECT_NEAR(presentation.progress_fraction, 1.0, 0.0001);
    EXPECT_EQ(presentation.css_class, "success");
}
