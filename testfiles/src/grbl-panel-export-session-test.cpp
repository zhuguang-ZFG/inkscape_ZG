// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-export-session.h"

using Inkscape::UI::Dialog::GrblPanelExportSources;
using Inkscape::UI::Dialog::make_grbl_panel_export_context;
using Inkscape::UI::Dialog::make_grbl_panel_export_session;

TEST(GrblPanelExportSessionTest, CopiesContextInputsIntoExportContext)
{
    std::atomic<bool> cancel{false};
    GrblPanelExportSources sources;
    sources.desktop = reinterpret_cast<SPDesktop *>(0x12);
    sources.selection = reinterpret_cast<Inkscape::Selection *>(0x34);
    sources.use_current_layer_without_selection = true;
    sources.cancel = &cancel;

    auto const context = make_grbl_panel_export_context(sources);

    EXPECT_EQ(context.desktop, sources.desktop);
    EXPECT_EQ(context.selection, sources.selection);
    EXPECT_TRUE(context.use_current_layer_without_selection);
    EXPECT_EQ(context.cancel, &cancel);
}

TEST(GrblPanelExportSessionTest, BundlesParamsAndContextTogether)
{
    Inkscape::Axidraw::GrblExportParams params;
    params.feed_draw_mm_min = 1234.0;
    params.clip_to_machine_bed = true;

    GrblPanelExportSources sources;
    sources.use_current_layer_without_selection = true;

    auto const session = make_grbl_panel_export_session(params, sources);

    EXPECT_DOUBLE_EQ(session.params.feed_draw_mm_min, 1234.0);
    EXPECT_TRUE(session.params.clip_to_machine_bed);
    EXPECT_TRUE(session.context.use_current_layer_without_selection);
    EXPECT_EQ(session.context.desktop, nullptr);
    EXPECT_EQ(session.context.selection, nullptr);
}
