// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include <gtest/gtest.h>

#include "src/axidraw/device/grbl-client.h"
#include "src/ui/dialog/grbl-panel-connect-attempt.h"

using Inkscape::Axidraw::GrblProbeResult;
using Inkscape::UI::Dialog::GrblPanelConnectAttempt;
using Inkscape::UI::Dialog::GrblPanelConnectAttemptOutcome;
using Inkscape::UI::Dialog::GrblPanelConnectRequest;
using Inkscape::UI::Dialog::describe_connect_open_failure_ui;
using Inkscape::UI::Dialog::describe_probe_failure_ui;
using Inkscape::UI::Dialog::describe_tcp_probe_failure_ui;
using Inkscape::UI::Dialog::make_connect_probe_status;
using Inkscape::UI::Dialog::parse_tcp_device_spec;
using Inkscape::UI::Dialog::serial_open_timeout_for_device;

TEST(GrblPanelConnectAttemptTest, ParsesIpv4AndIpv6TcpSpecs)
{
    std::string host;
    int port = 0;
    EXPECT_TRUE(parse_tcp_device_spec("tcp://192.168.1.9:23", host, port));
    EXPECT_EQ(host, "192.168.1.9");
    EXPECT_EQ(port, 23);

    host.clear();
    port = 0;
    EXPECT_TRUE(parse_tcp_device_spec("[fe80::1]:4567", host, port));
    EXPECT_EQ(host, "fe80::1");
    EXPECT_EQ(port, 4567);
}

TEST(GrblPanelConnectAttemptTest, RejectsInvalidTcpSpecs)
{
    std::string host = "keep";
    int port = 12;
    EXPECT_FALSE(parse_tcp_device_spec("bad", host, port));
    EXPECT_FALSE(parse_tcp_device_spec("tcp://host", host, port));
    EXPECT_FALSE(parse_tcp_device_spec("host:not-a-port", host, port));
    EXPECT_FALSE(parse_tcp_device_spec("host:70000", host, port));
}

TEST(GrblPanelConnectAttemptTest, ConnectionMessagesReflectTransportAndFailureType)
{
    EXPECT_NE(make_connect_probe_status("COM3", 115200, false).raw().find("115200"), std::string::npos);
    EXPECT_NE(make_connect_probe_status("tcp://plotter:23", 0, true).raw().find("TCP"), std::string::npos);

    auto const timeout = describe_connect_open_failure_ui(false, true, false);
    auto const access_denied = describe_connect_open_failure_ui(false, false, true);
    auto const tcp = describe_connect_open_failure_ui(true, false, false);
    EXPECT_NE(timeout.raw(), access_denied.raw());
    EXPECT_NE(access_denied.raw(), tcp.raw());
}

TEST(GrblPanelConnectAttemptTest, ProbeFailureMessagesPreserveResponseDetail)
{
    GrblProbeResult empty_probe{false, ""};
    GrblProbeResult noisy_probe{false, "HELLO"};

    auto const serial_empty = describe_probe_failure_ui("COM5", 115200, empty_probe);
    auto const serial_noisy = describe_probe_failure_ui("COM5", 115200, noisy_probe);
    auto const tcp_noisy = describe_tcp_probe_failure_ui("tcp://host:23", noisy_probe);

    EXPECT_FALSE(serial_empty.empty());
    EXPECT_NE(serial_empty.raw(), serial_noisy.raw());
    EXPECT_NE(serial_noisy.raw().find("HELLO"), std::string::npos);
    EXPECT_NE(tcp_noisy.raw().find("HELLO"), std::string::npos);
}

TEST(GrblPanelConnectAttemptTest, ComPortsUseLongerOpenTimeout)
{
    EXPECT_EQ(serial_open_timeout_for_device("COM9"), 12000);
    EXPECT_EQ(serial_open_timeout_for_device("/dev/ttyUSB0"), 6000);
}

TEST(GrblPanelConnectAttemptTest, PreCancelledAttemptReturnsCancelledWithoutOpeningPorts)
{
    GrblPanelConnectRequest request;
    request.device = "COM1";
    std::atomic<bool> stop{true};

    auto result = GrblPanelConnectAttempt::run(request, stop);
    EXPECT_EQ(result.outcome, GrblPanelConnectAttemptOutcome::cancelled);
    EXPECT_EQ(result.serial, nullptr);
    EXPECT_EQ(result.tcp, nullptr);
}
