// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-summary-presentation.h"

using Inkscape::UI::Dialog::build_grbl_fill_gcode_status;
using Inkscape::UI::Dialog::build_grbl_job_summary_markup;
using Inkscape::UI::Dialog::build_grbl_layout_scale_summary_markup;

TEST(GrblPanelSummaryPresentationTest, LayoutScaleSummaryShowsScaleAndUsage)
{
    Inkscape::Axidraw::GrblPlotStats stats;
    stats.has_bounds_mm = true;
    stats.min_x_mm = 0.0;
    stats.max_x_mm = 100.0;
    stats.min_y_mm = 0.0;
    stats.max_y_mm = 50.0;

    auto const markup = build_grbl_layout_scale_summary_markup(stats, 200.0, 100.0).raw();
    EXPECT_NE(markup.find("当前缩放"), std::string::npos);
    EXPECT_NE(markup.find("机器行程占用"), std::string::npos);
}

TEST(GrblPanelSummaryPresentationTest, JobSummaryIncludesCountsAndLengthsWhenAvailable)
{
    Inkscape::Axidraw::GrblPlotStats stats;
    stats.layer_count = 3;
    stats.stroke_count = 12;
    stats.tool_change_count = 1;
    stats.estimated_duration_sec = 125.0;
    stats.has_bounds_mm = true;
    stats.min_x_mm = 0.0;
    stats.max_x_mm = 100.0;
    stats.min_y_mm = 0.0;
    stats.max_y_mm = 50.0;
    stats.has_length_stats = true;
    stats.draw_length_mm = 80.0;
    stats.travel_length_mm = 20.0;

    auto const markup = build_grbl_job_summary_markup(stats, 200.0, 100.0).raw();
    EXPECT_NE(markup.find("任务概览"), std::string::npos);
    EXPECT_NE(markup.find("图层数"), std::string::npos);
    EXPECT_NE(markup.find("空走占比"), std::string::npos);
}

TEST(GrblPanelSummaryPresentationTest, FillGcodeStatusMentionsBoundsWhenAvailable)
{
    Inkscape::Axidraw::GrblPlotStats stats;
    stats.has_bounds_mm = true;
    stats.min_x_mm = 0.0;
    stats.max_x_mm = 100.0;
    stats.min_y_mm = 0.0;
    stats.max_y_mm = 50.0;

    auto const status = build_grbl_fill_gcode_status(7, stats).raw();
    EXPECT_NE(status.find("7"), std::string::npos);
    EXPECT_NE(status.find("mm"), std::string::npos);
}
