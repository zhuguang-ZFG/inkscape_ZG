// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-connection-state.h"

using Inkscape::UI::Dialog::GrblPanelConnectButtonAction;
using Inkscape::UI::Dialog::GrblRuntimePhase;
using Inkscape::UI::Dialog::make_connect_attempt_begin_plan;
using Inkscape::UI::Dialog::make_connect_attempt_failure_plan;
using Inkscape::UI::Dialog::make_connect_attempt_success_plan;
using Inkscape::UI::Dialog::make_disconnect_controller_plan;

TEST(GrblPanelConnectionStateTest, ConnectBeginStopsPollingAndCancelsDelayedSync)
{
    auto const plan = make_connect_attempt_begin_plan();

    EXPECT_TRUE(plan.set_connecting);
    EXPECT_TRUE(plan.cancel_delayed_firmware_sync);
    EXPECT_TRUE(plan.update_poll);
    EXPECT_FALSE(plan.poll_enabled);
    EXPECT_TRUE(plan.post_status);
}

TEST(GrblPanelConnectionStateTest, ConnectFailureOnlyTouchesActiveAttempt)
{
    auto const inactive = make_connect_attempt_failure_plan(false, true);
    EXPECT_FALSE(inactive.post_status);
    EXPECT_FALSE(inactive.clear_machine_status);
    EXPECT_EQ(inactive.connect_button, GrblPanelConnectButtonAction::none);

    auto const active = make_connect_attempt_failure_plan(true, true);
    EXPECT_TRUE(active.post_status);
    EXPECT_TRUE(active.status_is_error);
    EXPECT_TRUE(active.clear_machine_status);
    EXPECT_EQ(active.connect_button, GrblPanelConnectButtonAction::deactivate);
}

TEST(GrblPanelConnectionStateTest, ConnectSuccessEitherFinalizesUiOrDisconnects)
{
    auto const active = make_connect_attempt_success_plan(true);
    EXPECT_TRUE(active.post_status);
    EXPECT_TRUE(active.update_poll);
    EXPECT_TRUE(active.poll_enabled);
    EXPECT_TRUE(active.trigger_firmware_sync_now);
    EXPECT_TRUE(active.schedule_delayed_firmware_sync);
    EXPECT_TRUE(active.refresh_plot_feedback);
    EXPECT_FALSE(active.disconnect_link);

    auto const inactive = make_connect_attempt_success_plan(false);
    EXPECT_TRUE(inactive.disconnect_link);
    EXPECT_FALSE(inactive.post_status);
    EXPECT_FALSE(inactive.schedule_delayed_firmware_sync);
}

TEST(GrblPanelConnectionStateTest, DisconnectPlanStopsPollingAndClearsMachineStatus)
{
    auto const plan = make_disconnect_controller_plan();

    EXPECT_TRUE(plan.cancel_delayed_firmware_sync);
    EXPECT_TRUE(plan.update_poll);
    EXPECT_FALSE(plan.poll_enabled);
    EXPECT_TRUE(plan.clear_machine_status);
}
