// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-presentation.h"

using Inkscape::UI::Dialog::build_grbl_connection_status;
using Inkscape::UI::Dialog::build_grbl_default_gcode_filename;
using Inkscape::UI::Dialog::build_grbl_gcode_load_status;
using Inkscape::UI::Dialog::build_grbl_gcode_save_status;
using Inkscape::UI::Dialog::build_grbl_not_connected_status;
using Inkscape::UI::Dialog::grbl_gcode_text_has_content;
using Inkscape::UI::Dialog::make_grbl_action_buttons_presentation;
using Inkscape::UI::Dialog::make_grbl_connection_presentation;
using Inkscape::UI::Dialog::make_grbl_runtime_state_view;

TEST(GrblPanelPresentationTest, SendingStateUpdatesActionButtonCopy)
{
    auto const state = make_grbl_runtime_state_view(false, false, true, false);
    auto const presentation = make_grbl_action_buttons_presentation(state);

    EXPECT_EQ(presentation.send_gcode.label.raw(), "鍙戦€佷腑...");
    EXPECT_EQ(presentation.send_from_drawing.label.raw(), "鍥剧鍙戦€佷腑...");
    EXPECT_NE(presentation.send_gcode.tooltip.raw().find("鍙栨秷鍙戦€?"), std::string::npos);
    EXPECT_EQ(presentation.cancel_gcode.label.raw(), "鍙栨秷鍙戦€?_C)");
}

TEST(GrblPanelPresentationTest, CancellingStateUpdatesCancelButtonCopy)
{
    auto const state = make_grbl_runtime_state_view(false, false, true, true);
    auto const presentation = make_grbl_action_buttons_presentation(state);

    EXPECT_EQ(presentation.cancel_gcode.label.raw(), "鍋滄璇锋眰涓?..");
    EXPECT_NE(presentation.cancel_gcode.tooltip.raw().find("褰撳墠杩欎竴琛?"), std::string::npos);
}

TEST(GrblPanelPresentationTest, FirmwareSyncUpdatesReadButtonCopy)
{
    auto const state = make_grbl_runtime_state_view(false, true, false, false);
    auto const presentation = make_grbl_action_buttons_presentation(state);

    EXPECT_EQ(presentation.read_firmware.label.raw(), "鍚屾涓?..");
    EXPECT_NE(presentation.read_firmware.tooltip.raw().find("$I"), std::string::npos);
}

TEST(GrblPanelPresentationTest, ConnectionPresentationTracksPhaseAndConnection)
{
    auto const connecting = make_grbl_connection_presentation(
        make_grbl_runtime_state_view(true, false, false, false), false);
    EXPECT_FALSE(connecting.serial_controls_sensitive);
    EXPECT_EQ(connecting.connect.label.raw(), "杩炴帴涓?..");

    auto const connected = make_grbl_connection_presentation(
        make_grbl_runtime_state_view(false, false, false, false), true);
    EXPECT_TRUE(connected.serial_controls_sensitive);
    EXPECT_EQ(connected.connect.label.raw(), "鏂紑杩炴帴");

    auto const idle = make_grbl_connection_presentation(
        make_grbl_runtime_state_view(false, false, false, false), false);
    EXPECT_EQ(idle.connect.label.raw(), "杩炴帴");
    EXPECT_NE(idle.connect.tooltip.raw().find("鑷姩璇诲彇"), std::string::npos);
}

TEST(GrblPanelPresentationTest, ConnectionStatusIncludesProbeWhenPresent)
{
    auto const simple = build_grbl_connection_status("COM3");
    EXPECT_NE(simple.raw().find("COM3"), std::string::npos);

    auto const probed = build_grbl_connection_status("COM3", "Grbl 1.1");
    EXPECT_NE(probed.raw().find("Grbl 1.1"), std::string::npos);
}

TEST(GrblPanelPresentationTest, NotConnectedStatusVariesByTransportRequirement)
{
    EXPECT_NE(build_grbl_not_connected_status(true).raw().find("串口"), std::string::npos);
    EXPECT_NE(build_grbl_not_connected_status(false).raw().find("尚未连接"), std::string::npos);
}

TEST(GrblPanelPresentationTest, GcodeFilePresentationHelpersCoverCommonCases)
{
    EXPECT_FALSE(grbl_gcode_text_has_content("   \r\n\t "));
    EXPECT_TRUE(grbl_gcode_text_has_content("G1 X10"));
    EXPECT_EQ(build_grbl_default_gcode_filename(nullptr), "plot.nc");
    EXPECT_EQ(build_grbl_default_gcode_filename("D:/work/example.svg"), "example.nc");
    EXPECT_NE(build_grbl_gcode_load_status("demo.nc").raw().find("demo.nc"), std::string::npos);
    EXPECT_NE(build_grbl_gcode_save_status("demo.nc").raw().find("demo.nc"), std::string::npos);
}
