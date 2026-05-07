// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-firmware-sync-state.h"

using Inkscape::UI::Dialog::GrblFirmwareSnapshot;
using Inkscape::UI::Dialog::build_grbl_firmware_sync_status;

TEST(GrblPanelFirmwareSyncImportTest, FileImportStatusUsesImportSummary)
{
    GrblFirmwareSnapshot snapshot;
    snapshot.imported_from_file = true;
    snapshot.has_direction_mask = true;
    snapshot.direction_mask = 4;
    snapshot.has_x_travel = true;
    snapshot.x_travel_mm = 200.0;
    snapshot.has_y_travel = true;
    snapshot.y_travel_mm = 200.0;

    auto const status = build_grbl_firmware_sync_status(snapshot, false, false);
    EXPECT_NE(status.raw().find("导入"), std::string::npos);
    EXPECT_NE(status.raw().find("$3=4"), std::string::npos);
}

TEST(GrblPanelFirmwareSyncImportTest, FirmwareReadStatusKeepsReadSummary)
{
    GrblFirmwareSnapshot snapshot;
    snapshot.has_direction_mask = true;
    snapshot.direction_mask = 1;

    auto const status = build_grbl_firmware_sync_status(snapshot, false, false);
    EXPECT_NE(status.raw().find("读取"), std::string::npos);
    EXPECT_EQ(status.raw().find("导入"), std::string::npos);
}
