// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/axidraw/pipeline/grbl-export.h"
#include "src/document.h"
#include "src/inkscape.h"
#include "src/object/sp-item.h"
#include "src/selection.h"
#include "src/util/cast.h"

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Inkscape;
using namespace std::literals;

namespace {

std::vector<std::string> split_lines(std::string const &text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::optional<std::string> find_first_line_with_prefix(std::vector<std::string> const &lines, std::string const &prefix)
{
    for (auto const &line : lines) {
        if (line.rfind(prefix, 0) == 0) {
            return line;
        }
    }
    return std::nullopt;
}

std::unique_ptr<SPDocument> create_axidraw_doc(std::string_view body)
{
    auto const svg = std::string{R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
     width="100mm"
     height="100mm"
     viewBox="0 0 100 100">
  <sodipodi:namedview id="namedview1" inkscape:document-units="mm" />
)A"} + std::string(body) + "\n</svg>\n";

    auto doc = SPDocument::createNewDocFromMem(svg);
    if (doc) {
        doc->ensureUpToDate();
    }
    return doc;
}

class GrblExportTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { Inkscape::Application::create(false); }

    static std::string build_gcode(SPDocument *doc, Inkscape::Axidraw::GrblExportParams const &params,
                                   Inkscape::Axidraw::GrblExportContext const &ctx,
                                   std::size_t *stroke_count_out = nullptr)
    {
        std::string gcode;
        std::string err;
        if (!Inkscape::Axidraw::build_grbl_plot_gcode_string(doc, params, ctx, gcode, err, stroke_count_out)) {
            ADD_FAILURE() << err;
            return {};
        }
        return gcode;
    }
};

} // namespace

TEST_F(GrblExportTest, UsesConfiguredPenCommandsForZMode)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="p1" d="M 10,10 L 20,20" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    params.pen_up_cmd = "G1 Z0 F3000";
    params.pen_down_cmd = "G1 Z5 F3000";

    Inkscape::Axidraw::GrblExportContext ctx;
    std::size_t strokes = 0;
    auto const gcode = build_gcode(doc.get(), params, ctx, &strokes);
    auto const lines = split_lines(gcode);

    EXPECT_EQ(strokes, 1);
    EXPECT_EQ(lines[0], "G21");
    EXPECT_EQ(lines[1], "G90");
    EXPECT_NE(find_first_line_with_prefix(lines, "G1 Z0 F3000"), std::nullopt);
    EXPECT_NE(find_first_line_with_prefix(lines, "G1 Z5 F3000"), std::nullopt);
    EXPECT_NE(find_first_line_with_prefix(lines, "G0 X10.000 Y10.000 F6000"), std::nullopt);
    EXPECT_NE(find_first_line_with_prefix(lines, "G1 X20.000 Y20.000 F1200"), std::nullopt);
}

TEST_F(GrblExportTest, SupportsM3M5PenMode)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="p1" d="M 10,10 L 20,20" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    params.pen_up_cmd = "M5";
    params.pen_down_cmd = "M3 S1000";

    Inkscape::Axidraw::GrblExportContext ctx;
    auto const gcode = build_gcode(doc.get(), params, ctx);
    auto const lines = split_lines(gcode);

    EXPECT_NE(find_first_line_with_prefix(lines, "M5"), std::nullopt);
    EXPECT_NE(find_first_line_with_prefix(lines, "M3 S1000"), std::nullopt);
    EXPECT_EQ(find_first_line_with_prefix(lines, "G1 Z0 F3000"), std::nullopt);
    EXPECT_EQ(find_first_line_with_prefix(lines, "G1 Z5 F3000"), std::nullopt);
}

TEST_F(GrblExportTest, RestrictsExportToSelection)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="left" d="M 10,10 L 20,10" />
  <path id="right" d="M 70,80 L 80,80" />
)A");
    ASSERT_TRUE(doc);

    Selection selection(doc.get());
    auto *item = cast<SPItem>(doc->getObjectById("right"));
    ASSERT_TRUE(item);
    selection.add(item);

    Inkscape::Axidraw::GrblExportParams params;
    Inkscape::Axidraw::GrblExportContext ctx;
    ctx.selection = &selection;

    std::size_t strokes = 0;
    auto const gcode = build_gcode(doc.get(), params, ctx, &strokes);

    EXPECT_EQ(strokes, 1);
    EXPECT_NE(gcode.find("G0 X70.000 Y80.000 F6000"), std::string::npos);
    EXPECT_NE(gcode.find("G1 X80.000 Y80.000 F1200"), std::string::npos);
    EXPECT_EQ(gcode.find("X10.000 Y10.000"), std::string::npos);
    EXPECT_EQ(gcode.find("X20.000 Y10.000"), std::string::npos);
}

TEST_F(GrblExportTest, ReordersStrokesWhenOptimizationIsEnabled)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="far" d="M 80,0 L 90,0" />
  <path id="near" d="M 10,0 L 20,0" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportContext ctx;

    Inkscape::Axidraw::GrblExportParams original_order;
    original_order.optimize_stroke_order = false;
    auto const gcode_original = build_gcode(doc.get(), original_order, ctx);
    auto const first_g0_original = find_first_line_with_prefix(split_lines(gcode_original), "G0 ");
    ASSERT_TRUE(first_g0_original.has_value());
    EXPECT_EQ(first_g0_original->rfind("G0 X80.000 Y0.000 F6000", 0), 0);

    Inkscape::Axidraw::GrblExportParams reordered;
    reordered.optimize_stroke_order = true;
    auto const gcode_reordered = build_gcode(doc.get(), reordered, ctx);
    auto const first_g0_reordered = find_first_line_with_prefix(split_lines(gcode_reordered), "G0 ");
    ASSERT_TRUE(first_g0_reordered.has_value());
    EXPECT_EQ(first_g0_reordered->rfind("G0 X10.000 Y0.000 F6000", 0), 0);
}

TEST_F(GrblExportTest, AlignsContentToMachineOrigin)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="p1" d="M 10,20 L 30,40" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    params.align_content_min_to_origin = true;

    Inkscape::Axidraw::GrblExportContext ctx;
    auto const gcode = build_gcode(doc.get(), params, ctx);

    EXPECT_NE(gcode.find("G0 X0.000 Y0.000 F6000"), std::string::npos);
    EXPECT_NE(gcode.find("G1 X20.000 Y20.000 F1200"), std::string::npos);
    EXPECT_EQ(gcode.find("X10.000 Y20.000"), std::string::npos);
}

TEST_F(GrblExportTest, ClipsGeometryToMachineBed)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="p1" d="M -10,10 L 20,10" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    params.clip_to_machine_bed = true;
    params.machine_bed_width_mm = 15.0;
    params.machine_bed_depth_mm = 50.0;

    Inkscape::Axidraw::GrblExportContext ctx;
    auto const gcode = build_gcode(doc.get(), params, ctx);

    EXPECT_NE(gcode.find("G0 X0.000 Y10.000 F6000"), std::string::npos);
    EXPECT_NE(gcode.find("G1 X15.000 Y10.000 F1200"), std::string::npos);
    EXPECT_EQ(gcode.find("X-10.000 Y10.000"), std::string::npos);
    EXPECT_EQ(gcode.find("X20.000 Y10.000"), std::string::npos);
}

TEST_F(GrblExportTest, FlipsYUsingPageHeight)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="p1" d="M 10,20 L 30,40" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    params.flip_y_canvas = true;

    Inkscape::Axidraw::GrblExportContext ctx;
    auto const gcode = build_gcode(doc.get(), params, ctx);

    EXPECT_NE(gcode.find("G0 X10.000 Y80.000 F6000"), std::string::npos);
    EXPECT_NE(gcode.find("G1 X30.000 Y60.000 F1200"), std::string::npos);
    EXPECT_EQ(gcode.find("X10.000 Y20.000"), std::string::npos);
    EXPECT_EQ(gcode.find("X30.000 Y40.000"), std::string::npos);
}

TEST_F(GrblExportTest, MachinePreviewReflectsMappedCoordinates)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="p1" d="M 10,20 L 30,40" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    params.flip_y_canvas = true;
    params.align_content_min_to_origin = true;

    Inkscape::Axidraw::GrblExportContext ctx;
    Geom::PathVector preview;
    std::string err;
    bool approximate_due_to_clip = false;
    std::size_t included = 0;
    std::size_t total = 0;

    ASSERT_TRUE(Inkscape::Axidraw::build_grbl_plot_machine_preview_pathvector_in_doc_space(
        doc.get(), params, ctx, preview, err, &approximate_due_to_clip, 32, &included, &total))
        << err;

    ASSERT_FALSE(approximate_due_to_clip);
    ASSERT_EQ(included, 1);
    ASSERT_EQ(total, 1);
    ASSERT_EQ(preview.size(), 1);

    auto const initial = preview.front().initialPoint();
    auto const final = preview.front().finalPoint();

    EXPECT_NEAR(initial[Geom::X], 10.0, 1e-6);
    EXPECT_NEAR(initial[Geom::Y], 20.0, 1e-6);
    EXPECT_NEAR(final[Geom::X], 30.0, 1e-6);
    EXPECT_NEAR(final[Geom::Y], 40.0, 1e-6);
}

TEST_F(GrblExportTest, MachinePreviewReportsApproximationWhenClipped)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="p1" d="M -10,10 L 20,10" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    params.clip_to_machine_bed = true;
    params.machine_bed_width_mm = 15.0;
    params.machine_bed_depth_mm = 50.0;

    Inkscape::Axidraw::GrblExportContext ctx;
    Geom::PathVector preview;
    std::string err;
    bool approximate_due_to_clip = false;

    ASSERT_TRUE(Inkscape::Axidraw::build_grbl_plot_machine_preview_pathvector_in_doc_space(
        doc.get(), params, ctx, preview, err, &approximate_due_to_clip))
        << err;

    EXPECT_TRUE(approximate_due_to_clip);
    ASSERT_EQ(preview.size(), 1);
    EXPECT_NEAR(preview.front().initialPoint()[Geom::X], 0.0, 1e-6);
    EXPECT_NEAR(preview.front().finalPoint()[Geom::X], 15.0, 1e-6);
}
