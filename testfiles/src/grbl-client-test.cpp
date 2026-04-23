// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/axidraw/device/grbl-client.h"

TEST(GrblClientTest, ErrorLineDetectionIsCaseInsensitive)
{
    EXPECT_TRUE(Inkscape::Axidraw::grbl_is_error_line("error:1"));
    EXPECT_TRUE(Inkscape::Axidraw::grbl_is_error_line("ERROR:2"));
    EXPECT_TRUE(Inkscape::Axidraw::grbl_is_error_line("Alarm then ErRoR text"));
    EXPECT_FALSE(Inkscape::Axidraw::grbl_is_error_line("ok"));
    EXPECT_FALSE(Inkscape::Axidraw::grbl_is_error_line("<Idle|MPos:0,0,0>"));
}
