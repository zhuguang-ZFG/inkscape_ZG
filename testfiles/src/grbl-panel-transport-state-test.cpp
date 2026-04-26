// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-transport-state.h"

using Inkscape::UI::Dialog::GrblPanelLinkActivityState;
using Inkscape::UI::Dialog::make_transport_plan_for_firmware_sync_complete;
using Inkscape::UI::Dialog::make_transport_plan_for_firmware_sync_start;
using Inkscape::UI::Dialog::make_transport_plan_for_gcode_stream;

TEST(GrblPanelTransportStateTest, FirmwareSyncStartStopsPollingAndMarksActivity)
{
    auto const plan = make_transport_plan_for_firmware_sync_start(true);
    EXPECT_EQ(plan.activity, GrblPanelLinkActivityState::firmware_sync);
    EXPECT_TRUE(plan.update_poll);
    EXPECT_FALSE(plan.poll_enabled);
}

TEST(GrblPanelTransportStateTest, FirmwareSyncCompleteResumesPollingOnlyForActiveConnection)
{
    auto const connected = make_transport_plan_for_firmware_sync_complete(true, true, true);
    EXPECT_EQ(connected.activity, GrblPanelLinkActivityState::idle);
    EXPECT_TRUE(connected.update_poll);
    EXPECT_TRUE(connected.poll_enabled);

    auto const idle = make_transport_plan_for_firmware_sync_complete(true, true, false);
    EXPECT_EQ(idle.activity, GrblPanelLinkActivityState::idle);
    EXPECT_FALSE(idle.update_poll);
}

TEST(GrblPanelTransportStateTest, GcodeStreamStartStopsPollingAndUsesStreamingActivity)
{
    auto const plan = make_transport_plan_for_gcode_stream(true, true, true, false);
    EXPECT_EQ(plan.activity, GrblPanelLinkActivityState::streaming);
    EXPECT_TRUE(plan.update_poll);
    EXPECT_FALSE(plan.poll_enabled);
}

TEST(GrblPanelTransportStateTest, GcodeStreamFinishResumesOnlyWhenNotFirmwareSyncing)
{
    auto const connected = make_transport_plan_for_gcode_stream(false, true, true, false);
    EXPECT_EQ(connected.activity, GrblPanelLinkActivityState::idle);
    EXPECT_TRUE(connected.update_poll);
    EXPECT_TRUE(connected.poll_enabled);

    auto const disconnected = make_transport_plan_for_gcode_stream(false, true, false, false);
    EXPECT_EQ(disconnected.activity, GrblPanelLinkActivityState::idle);
    EXPECT_FALSE(disconnected.update_poll);

    auto const syncing = make_transport_plan_for_gcode_stream(false, true, true, true);
    EXPECT_EQ(syncing.activity, GrblPanelLinkActivityState::unchanged);
    EXPECT_FALSE(syncing.update_poll);
}
