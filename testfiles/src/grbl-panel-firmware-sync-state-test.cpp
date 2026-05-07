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
using Inkscape::UI::Dialog::parse_grbl_properties_snapshot;

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

TEST(GrblPanelFirmwareSyncStateTest, ImportedFileStatusUsesImportSummary)
{
    GrblFirmwareSnapshot snapshot;
    snapshot.imported_from_file = true;
    snapshot.has_x_travel = true;
    snapshot.x_travel_mm = 200.0;

    auto const status = build_grbl_firmware_sync_status(snapshot, false, false);
    EXPECT_NE(status.raw().find("导入"), std::string::npos);
    EXPECT_EQ(status.raw().find("已读取固件参数"), std::string::npos);
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

TEST(GrblPanelFirmwareSyncStateTest, ConnectSuccessKeepsDelayedSyncWhenStreamingBlocksImmediateStart)
{
    auto const blocked = make_grbl_firmware_sync_request_plan(
        GrblFirmwareSyncRequestOrigin::connect_success, true, GrblRuntimePhase::gcode_sending);

    EXPECT_FALSE(blocked.start_now);
    EXPECT_TRUE(blocked.keep_delayed_request);
}

TEST(GrblPanelFirmwareSyncStateTest, DelayedConnectNeverStartsAfterDisconnect)
{
    auto const plan = make_grbl_firmware_sync_request_plan(
        GrblFirmwareSyncRequestOrigin::delayed_connect, false, GrblRuntimePhase::idle);

    EXPECT_FALSE(plan.start_now);
    EXPECT_FALSE(plan.keep_delayed_request);
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

TEST(GrblPanelFirmwareSyncStateTest, ParseAcceptsRawDollarDump)
{
    std::string const text = "ok\r\n$3=4\r\n$130=410.000\r\n$131=260.500\r\nok\r\n";
    GrblFirmwareSnapshot snapshot;
    Glib::ustring error;
    ASSERT_TRUE(parse_grbl_properties_snapshot(text, snapshot, error));
    EXPECT_TRUE(snapshot.imported_from_file);
    EXPECT_TRUE(snapshot.has_direction_mask);
    EXPECT_EQ(snapshot.direction_mask, 4);
    EXPECT_TRUE(snapshot.has_x_travel);
    EXPECT_DOUBLE_EQ(snapshot.x_travel_mm, 410.0);
    EXPECT_TRUE(snapshot.has_y_travel);
    EXPECT_DOUBLE_EQ(snapshot.y_travel_mm, 260.5);
    EXPECT_NE(snapshot.display_text.raw().find("$130=410.000"), std::string::npos);
}

TEST(GrblPanelFirmwareSyncStateTest, ParseAcceptsPropertiesStyleAndIgnoresComments)
{
    std::string const text = "# header\n130=200\n!skip\n3=2\n";
    GrblFirmwareSnapshot snapshot;
    Glib::ustring error;
    ASSERT_TRUE(parse_grbl_properties_snapshot(text, snapshot, error));
    EXPECT_TRUE(snapshot.has_x_travel);
    EXPECT_DOUBLE_EQ(snapshot.x_travel_mm, 200.0);
    EXPECT_TRUE(snapshot.has_direction_mask);
    EXPECT_EQ(snapshot.direction_mask, 2);
    EXPECT_FALSE(snapshot.has_y_travel);
}

TEST(GrblPanelFirmwareSyncStateTest, ParseRejectsFileWithoutSettings)
{
    std::string const text = "ok\nerror: bad input\n";
    GrblFirmwareSnapshot snapshot;
    Glib::ustring error;
    EXPECT_FALSE(parse_grbl_properties_snapshot(text, snapshot, error));
    EXPECT_FALSE(error.empty());
}
