// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <2geom/rect.h>

#include "src/ui/dialog/grbl-panel-document-layout.h"

using Inkscape::UI::Dialog::build_grbl_fit_to_bed_status;
using Inkscape::UI::Dialog::make_grbl_center_to_bed_plan;
using Inkscape::UI::Dialog::make_grbl_fit_to_bed_plan;

TEST(GrblPanelDocumentLayoutTest, FitPlanShrinksAndMovesToOrigin)
{
    auto const bounds = Geom::Rect::from_xywh(Geom::Point(10, 20), Geom::Point(200, 50));
    auto const plan = make_grbl_fit_to_bed_plan(bounds, 100, 100);

    ASSERT_TRUE(plan.valid);
    EXPECT_TRUE(plan.shrink_applied);
    EXPECT_DOUBLE_EQ(plan.scale, 0.5);
    EXPECT_DOUBLE_EQ(plan.translate_x, -10);
    EXPECT_DOUBLE_EQ(plan.translate_y, -20);
}

TEST(GrblPanelDocumentLayoutTest, FitPlanDoesNotUpscaleSmallContent)
{
    auto const bounds = Geom::Rect::from_xywh(Geom::Point(5, 7), Geom::Point(20, 30));
    auto const plan = make_grbl_fit_to_bed_plan(bounds, 100, 120);

    ASSERT_TRUE(plan.valid);
    EXPECT_FALSE(plan.shrink_applied);
    EXPECT_DOUBLE_EQ(plan.scale, 1.0);
    EXPECT_DOUBLE_EQ(plan.translate_x, -5);
    EXPECT_DOUBLE_EQ(plan.translate_y, -7);
    auto const status = build_grbl_fit_to_bed_status(plan.scale, plan.shrink_applied).raw();
    EXPECT_NE(status.find("已仅将其移动"), std::string::npos);
}

TEST(GrblPanelDocumentLayoutTest, CenterPlanMovesContentToBedCenter)
{
    auto const bounds = Geom::Rect::from_xywh(Geom::Point(10, 20), Geom::Point(40, 20));
    auto const plan = make_grbl_center_to_bed_plan(bounds, 100, 80);

    ASSERT_TRUE(plan.valid);
    EXPECT_DOUBLE_EQ(plan.translate_x, 20);
    EXPECT_DOUBLE_EQ(plan.translate_y, 10);
}

TEST(GrblPanelDocumentLayoutTest, InvalidPlanRejectedForEmptyBounds)
{
    auto const bounds = Geom::Rect::from_xywh(Geom::Point(0, 0), Geom::Point(0, 20));
    EXPECT_FALSE(make_grbl_fit_to_bed_plan(bounds, 100, 80).valid);
    EXPECT_FALSE(make_grbl_center_to_bed_plan(bounds, 100, 80).valid);
}
