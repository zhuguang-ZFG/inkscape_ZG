// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/axidraw/device/grbl-client.h"

#include <string>
#include <vector>

TEST(GrblClientTest, ErrorLineDetectionIsCaseInsensitive)
{
    EXPECT_TRUE(Inkscape::Axidraw::grbl_is_error_line("error:1"));
    EXPECT_TRUE(Inkscape::Axidraw::grbl_is_error_line("ERROR:2"));
    EXPECT_TRUE(Inkscape::Axidraw::grbl_is_error_line("  ErRoR:15"));
    EXPECT_FALSE(Inkscape::Axidraw::grbl_is_error_line("ok"));
    EXPECT_FALSE(Inkscape::Axidraw::grbl_is_error_line("<Idle|MPos:0,0,0>"));
    EXPECT_FALSE(Inkscape::Axidraw::grbl_is_error_line("Alarm then ErRoR text"));
    EXPECT_FALSE(Inkscape::Axidraw::grbl_is_error_line("[MSG:Last error cleared]"));
}

TEST(GrblClientTest, SendLinesStopsAtFirstFailure)
{
    std::vector<std::string> sent;
    std::string err;

    auto const ok = Inkscape::Axidraw::grbl_send_lines(
        [&](std::string const &line, std::string &err_out) {
            sent.push_back(line);
            if (line == "G91") {
                err_out = "error:20";
                return false;
            }
            return true;
        },
        {"G21", "G91", "G0 X0 Y0"}, err);

    EXPECT_FALSE(ok);
    EXPECT_EQ(err, "error:20");
    EXPECT_EQ(sent, (std::vector<std::string>{"G21", "G91"}));
}

TEST(GrblClientTest, SendLinesReturnsTrueWhenAllCommandsSucceed)
{
    std::vector<std::string> sent;
    std::string err = "stale";

    auto const ok = Inkscape::Axidraw::grbl_send_lines(
        [&](std::string const &line, std::string &) {
            sent.push_back(line);
            return true;
        },
        {"G21", "G90", "G0 X0 Y0"}, err);

    EXPECT_TRUE(ok);
    EXPECT_EQ(err, "stale");
    EXPECT_EQ(sent, (std::vector<std::string>{"G21", "G90", "G0 X0 Y0"}));
}
