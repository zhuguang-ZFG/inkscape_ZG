// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/axidraw/device/grbl-link.h"
#include "src/axidraw/device/serial-port.h"
#include "src/axidraw/device/tcp-port.h"

#include <memory>
#include <string>

using Inkscape::Axidraw::GrblLink;

TEST(GrblLinkTest, KindTracksAssignedTransport)
{
    GrblLink link;
    EXPECT_EQ(link.kind(), GrblLink::Kind::none);

    link.set_serial(std::make_unique<Inkscape::Axidraw::SerialPort>());
    EXPECT_EQ(link.kind(), GrblLink::Kind::serial);

    link.set_tcp(std::make_unique<Inkscape::Axidraw::TcpPort>());
    EXPECT_EQ(link.kind(), GrblLink::Kind::tcp);

    link.close();
    EXPECT_EQ(link.kind(), GrblLink::Kind::none);
}

TEST(GrblLinkTest, CloseResetsActivityToIdle)
{
    GrblLink link;
    link.set_activity(GrblLink::Activity::streaming);
    EXPECT_EQ(link.activity(), GrblLink::Activity::streaming);

    link.close();
    EXPECT_EQ(link.activity(), GrblLink::Activity::idle);
}

TEST(GrblLinkTest, SendLineWaitOkReturnsNotConnectedWhenIdleAndUnbound)
{
    GrblLink link;
    std::string err;

    EXPECT_FALSE(link.send_line_wait_ok("G0 X1 Y1", err));
    EXPECT_EQ(err, "not connected");
}

TEST(GrblLinkTest, StreamingBlocksQueryCommandsWithExplicitError)
{
    GrblLink link;
    link.set_activity(GrblLink::Activity::streaming);

    std::string err;
    EXPECT_FALSE(link.send_line_wait_ok("$I", err));
    EXPECT_EQ(err, "blocked during streaming");

    EXPECT_FALSE(link.send_line_wait_ok("  $$  ", err));
    EXPECT_EQ(err, "blocked during streaming");

    EXPECT_FALSE(link.send_line_wait_ok("?", err));
    EXPECT_EQ(err, "blocked during streaming");

    EXPECT_FALSE(link.send_line_wait_ok(" $G ", err));
    EXPECT_EQ(err, "blocked during streaming");

    EXPECT_FALSE(link.send_line_wait_ok("\t$#\t", err));
    EXPECT_EQ(err, "blocked during streaming");
}

TEST(GrblLinkTest, StreamingBlocksSingleQuestionMarkByteWrite)
{
    GrblLink link;
    link.set_activity(GrblLink::Activity::streaming);

    unsigned char query = '?';
    EXPECT_FALSE(link.write_bytes(&query, 1));
}

TEST(GrblLinkTest, IdleDoesNotBlockQueryCommands)
{
    GrblLink link;
    link.set_activity(GrblLink::Activity::idle);

    std::string err;
    EXPECT_FALSE(link.send_line_wait_ok("?", err));
    EXPECT_EQ(err, "not connected");
}

TEST(GrblLinkTest, FirmwareSyncDoesNotBlockQueryCommands)
{
    GrblLink link;
    link.set_activity(GrblLink::Activity::firmware_sync);

    std::string err;
    EXPECT_FALSE(link.send_line_wait_ok("$$", err));
    EXPECT_EQ(err, "not connected");
}

