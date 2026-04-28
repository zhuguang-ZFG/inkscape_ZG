// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/axidraw/core/plot-orchestrator-messages.h"
#include "src/axidraw/device/grbl-client.h"

TEST(PlotOrchestratorMessagesTest, OpenFailureMessageMentionsTimeoutWhenTimedOut)
{
    auto const msg = Inkscape::Axidraw::describe_open_failure("COM4", 115200, true);
    EXPECT_NE(msg.find("Timed out while opening serial port"), std::string::npos);
    EXPECT_NE(msg.find("COM4"), std::string::npos);
    EXPECT_NE(msg.find("115200"), std::string::npos);
}

TEST(PlotOrchestratorMessagesTest, OpenFailureMessageUsesNonTimeoutText)
{
    auto const msg = Inkscape::Axidraw::describe_open_failure("COM7", 9600, false);
    EXPECT_NE(msg.find("Could not open serial port"), std::string::npos);
    EXPECT_NE(msg.find("COM7"), std::string::npos);
    EXPECT_NE(msg.find("9600"), std::string::npos);
}

TEST(PlotOrchestratorMessagesTest, ProbeFailureMessageCoversEmptyResponse)
{
    Inkscape::Axidraw::GrblProbeResult probe{};
    probe.ok = false;
    probe.response_line.clear();

    auto const msg = Inkscape::Axidraw::describe_probe_failure("COM5", 57600, probe);
    EXPECT_NE(msg.find("No GRBL status response"), std::string::npos);
    EXPECT_NE(msg.find("COM5"), std::string::npos);
    EXPECT_NE(msg.find("57600"), std::string::npos);
}

TEST(PlotOrchestratorMessagesTest, ProbeFailureMessageIncludesUnexpectedReply)
{
    Inkscape::Axidraw::GrblProbeResult probe{};
    probe.ok = false;
    probe.response_line = "HELLO";

    auto const msg = Inkscape::Axidraw::describe_probe_failure("COM9", 115200, probe);
    EXPECT_NE(msg.find("answered, but not like a GRBL controller"), std::string::npos);
    EXPECT_NE(msg.find("HELLO"), std::string::npos);
}

