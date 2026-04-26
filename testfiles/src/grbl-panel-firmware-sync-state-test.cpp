// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-firmware-sync-state.h"

using Inkscape::UI::Dialog::GrblFirmwareSnapshot;
using Inkscape::UI::Dialog::GrblFirmwareSyncApplyResult;
using Inkscape::UI::Dialog::make_grbl_firmware_mapping_update;
using Inkscape::UI::Dialog::GrblFirmwareSyncRequestOrigin;
using Inkscape::UI::Dialog::GrblRuntimePhase;
using Inkscape::UI::Dialog::build_grbl_firmware_sync_status;
using Inkscape::UI::Dialog::can_start_grbl_firmware_sync;
using Inkscape::UI::Dialog::make_grbl_firmware_sync_request_plan;
using Inkscape::UI::Dialog::make_grbl_firmware_sync_ui_plan;

TEST(GrblPanelFirmwareSyncStateTest, SyncStartIsBlockedOnlyWhileStreaming)
{
    EXPECT_TRUE(can_start_grbl_firmware_sync(GrblRuntimePhase::idle));
    EXPECT_TRUE(can_start_grbl_firmware_sync(GrblRuntimePhase::connecting));
    EXPECT_TRUE(can_start_grbl_firmware_sync(GrblRuntimePhase::firmware_sync));
    EXPECT_FALSE(can_start_grbl_firmware_sync(GrblRuntimePhase::gcode_sending));
    EXPECT_FALSE(can_start_grbl_firmware_sync(GrblRuntimePhase::gcode_cancelling));
}

TEST(GrblPanelFirmwareSyncStateTest, StatusMentionsSyncedFields)
{
    GrblFirmwareSnapshot snapshot;
    snapshot.has_direction_mask = true;
    snapshot.direction_mask = 3;
    snapshot.has_x_travel = true;
    snapshot.has_y_travel = true;
    snapshot.x_travel_mm = 410.0;
    snapshot.y_travel_mm = 260.0;

    auto const status = build_grbl_firmware_sync_status(snapshot, true, true);
    EXPECT_NE(status.raw().find("$3=3"), std::string::npos);
    EXPECT_NE(status.raw().find("410"), std::string::npos);
    EXPECT_NE(status.raw().find("260"), std::string::npos);
    EXPECT_NE(status.raw().find("mm"), std::string::npos);
}

TEST(GrblPanelFirmwareSyncStateTest, RequestPlanUnifiesManualAndConnectTriggers)
{
    auto const manual = make_grbl_firmware_sync_request_plan(
        GrblFirmwareSyncRequestOrigin::manual, true, GrblRuntimePhase::idle);
    EXPECT_TRUE(manual.start_now);
    EXPECT_FALSE(manual.keep_delayed_request);

    auto const connect = make_grbl_firmware_sync_request_plan(
        GrblFirmwareSyncRequestOrigin::connect_success, true, GrblRuntimePhase::idle);
    EXPECT_TRUE(connect.start_now);
    EXPECT_TRUE(connect.keep_delayed_request);

    auto const delayed_busy = make_grbl_firmware_sync_request_plan(
        GrblFirmwareSyncRequestOrigin::delayed_connect, true, GrblRuntimePhase::connecting);
    EXPECT_FALSE(delayed_busy.start_now);
    EXPECT_FALSE(delayed_busy.keep_delayed_request);
}

TEST(GrblPanelFirmwareSyncStateTest, MappingUpdateExtractsUiRelevantValues)
{
    GrblFirmwareSnapshot snapshot;
    snapshot.has_direction_mask = true;
    snapshot.direction_mask = 3;
    snapshot.has_x_travel = true;
    snapshot.x_travel_mm = 410.0;

    auto const update = make_grbl_firmware_mapping_update(snapshot);
    EXPECT_TRUE(update.has_invert_x);
    EXPECT_TRUE(update.invert_x);
    EXPECT_TRUE(update.has_invert_y);
    EXPECT_TRUE(update.invert_y);
    EXPECT_TRUE(update.has_bed_width);
    EXPECT_DOUBLE_EQ(update.bed_width, 410.0);
    EXPECT_FALSE(update.has_bed_depth);
}

TEST(GrblPanelFirmwareSyncStateTest, UiPlanChoosesBetweenSavingPrefsAndRefresh)
{
    GrblFirmwareSnapshot snapshot;
    GrblFirmwareSyncApplyResult changed;
    changed.changed = true;

    auto const changed_plan = make_grbl_firmware_sync_ui_plan(snapshot, changed);
    EXPECT_TRUE(changed_plan.save_mapping_preferences);
    EXPECT_FALSE(changed_plan.schedule_plot_feedback_refresh);
    EXPECT_FALSE(changed_plan.status.empty());

    GrblFirmwareSyncApplyResult unchanged;
    auto const unchanged_plan = make_grbl_firmware_sync_ui_plan(snapshot, unchanged);
    EXPECT_FALSE(unchanged_plan.save_mapping_preferences);
    EXPECT_TRUE(unchanged_plan.schedule_plot_feedback_refresh);
}
