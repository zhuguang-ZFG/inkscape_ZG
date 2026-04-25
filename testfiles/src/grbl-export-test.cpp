// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/axidraw/pipeline/grbl-export.h"
#include "src/document.h"
#include "src/inkscape.h"
#include "src/object/sp-item-group.h"
#include "src/object/sp-item.h"
#include "src/selection.h"
#include "src/util/cast.h"
#include "src/util/units.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Inkscape;
using namespace std::literals;

namespace Inkscape::Util {

std::string UnitTable::getUnitsFilename()
{
    if (auto const *datadir = std::getenv("INKSCAPE_DATADIR"); datadir && *datadir) {
        return std::string(datadir) + "/ui/units.xml";
    }
    return {};
}

} // namespace Inkscape::Util

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

std::string trim(std::string s)
{
    auto const first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    auto const last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string read_text_file(std::string const &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool write_text_file(std::string const &path, std::string const &text)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

struct GcodeMetrics {
    std::size_t total_lines = 0;
    std::size_t nonempty_lines = 0;
    std::size_t g0_count = 0;
    std::size_t g1_count = 0;
    std::size_t g4_count = 0;
    std::size_t m3_count = 0;
    std::size_t m5_count = 0;
    std::size_t m6_count = 0;
    std::size_t pen_up_like_count = 0;
    std::size_t pen_down_like_count = 0;
    std::size_t repeated_motion_target_count = 0;
    std::size_t zero_length_g0_count = 0;
    std::size_t zero_length_g1_count = 0;
    double min_draw_segment_mm = std::numeric_limits<double>::infinity();
    double max_draw_segment_mm = 0.0;
    double min_travel_segment_mm = std::numeric_limits<double>::infinity();
    double max_travel_segment_mm = 0.0;
};

std::optional<double> parse_axis(std::string const &line, char axis)
{
    std::regex const re(std::string(R"(\b)") + axis + R"(([+-]?(?:\d+(?:\.\d*)?|\.\d+)))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(line, match, re)) {
        return std::nullopt;
    }
    try {
        return std::stod(match[1].str());
    } catch (...) {
        return std::nullopt;
    }
}

void update_motion_extrema(double value, double &min_value, double &max_value)
{
    min_value = std::min(min_value, value);
    max_value = std::max(max_value, value);
}

GcodeMetrics analyze_gcode_metrics(std::string const &gcode, Inkscape::Axidraw::GrblExportParams const &params)
{
    GcodeMetrics metrics;
    std::optional<double> prev_x;
    std::optional<double> prev_y;

    for (auto const &raw_line : split_lines(gcode)) {
        ++metrics.total_lines;
        auto const line = trim(raw_line);
        if (line.empty()) {
            continue;
        }

        ++metrics.nonempty_lines;
        if (line.rfind("G0", 0) == 0) {
            ++metrics.g0_count;
        } else if (line.rfind("G1", 0) == 0) {
            ++metrics.g1_count;
        } else if (line.rfind("G4", 0) == 0) {
            ++metrics.g4_count;
        } else if (line.rfind("M3", 0) == 0) {
            ++metrics.m3_count;
        } else if (line.rfind("M5", 0) == 0) {
            ++metrics.m5_count;
        }

        if (line.find(" M6") != std::string::npos || line.rfind("M6", 0) == 0) {
            ++metrics.m6_count;
        }
        if (line == params.pen_up_cmd.raw() || line == "M5") {
            ++metrics.pen_up_like_count;
        }
        if (line == params.pen_down_cmd.raw() || line.rfind("M3", 0) == 0) {
            ++metrics.pen_down_like_count;
        }

        if (!(line.rfind("G0", 0) == 0 || line.rfind("G1", 0) == 0)) {
            continue;
        }

        auto const x = parse_axis(line, 'X');
        auto const y = parse_axis(line, 'Y');
        if (!x || !y || !prev_x || !prev_y) {
            if (x) {
                prev_x = *x;
            }
            if (y) {
                prev_y = *y;
            }
            continue;
        }

        double const dx = *x - *prev_x;
        double const dy = *y - *prev_y;
        double const length = std::hypot(dx, dy);
        bool const is_zero = std::abs(dx) < 1e-9 && std::abs(dy) < 1e-9;
        if (is_zero) {
            ++metrics.repeated_motion_target_count;
            if (line.rfind("G0", 0) == 0) {
                ++metrics.zero_length_g0_count;
            } else {
                ++metrics.zero_length_g1_count;
            }
        }
        if (line.rfind("G0", 0) == 0) {
            update_motion_extrema(length, metrics.min_travel_segment_mm, metrics.max_travel_segment_mm);
        } else {
            update_motion_extrema(length, metrics.min_draw_segment_mm, metrics.max_draw_segment_mm);
        }

        prev_x = *x;
        prev_y = *y;
    }

    if (!std::isfinite(metrics.min_draw_segment_mm)) {
        metrics.min_draw_segment_mm = 0.0;
    }
    if (!std::isfinite(metrics.min_travel_segment_mm)) {
        metrics.min_travel_segment_mm = 0.0;
    }
    return metrics;
}

std::size_t count_raw_layers(std::string const &svg_text)
{
    std::regex const layer_re(R"(<g\b[^>]*\binkscape:groupmode\s*=\s*"layer"[^>]*>)", std::regex::icase);
    return static_cast<std::size_t>(std::distance(std::sregex_iterator(svg_text.begin(), svg_text.end(), layer_re),
                                                  std::sregex_iterator()));
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
    auto const first_g0 = find_first_line_with_prefix(lines, "G0 ");
    auto const first_g1 = find_first_line_with_prefix(lines, "G1 X");
    ASSERT_TRUE(first_g0.has_value());
    ASSERT_TRUE(first_g1.has_value());
    auto const g0x = parse_axis(*first_g0, 'X');
    auto const g0y = parse_axis(*first_g0, 'Y');
    auto const g1x = parse_axis(*first_g1, 'X');
    auto const g1y = parse_axis(*first_g1, 'Y');
    ASSERT_TRUE(g0x && g0y && g1x && g1y);
    EXPECT_LT(*g0x, *g1x);
    EXPECT_LT(*g0y, *g1y);
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
    auto const lines = split_lines(gcode);
    auto const first_g0 = find_first_line_with_prefix(lines, "G0 ");
    auto const first_g1 = find_first_line_with_prefix(lines, "G1 X");
    ASSERT_TRUE(first_g0.has_value());
    ASSERT_TRUE(first_g1.has_value());
    auto const g0x = parse_axis(*first_g0, 'X');
    auto const g1x = parse_axis(*first_g1, 'X');
    ASSERT_TRUE(g0x && g1x);

    EXPECT_EQ(strokes, 1);
    EXPECT_GT(*g0x, 50.0);
    EXPECT_GT(*g1x, *g0x);
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
    auto const original_x = parse_axis(*first_g0_original, 'X');
    ASSERT_TRUE(original_x.has_value());

    Inkscape::Axidraw::GrblExportParams reordered;
    reordered.optimize_stroke_order = true;
    auto const gcode_reordered = build_gcode(doc.get(), reordered, ctx);
    auto const first_g0_reordered = find_first_line_with_prefix(split_lines(gcode_reordered), "G0 ");
    ASSERT_TRUE(first_g0_reordered.has_value());
    auto const reordered_x = parse_axis(*first_g0_reordered, 'X');
    ASSERT_TRUE(reordered_x.has_value());
    EXPECT_GT(*original_x, *reordered_x);
}

TEST_F(GrblExportTest, SkipsRedundantTravelMoveWhenNextStrokeStartsAtCurrentPosition)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="a" d="M 10,10 L 20,10" />
  <path id="b" d="M 20,10 L 30,10" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    Inkscape::Axidraw::GrblExportContext ctx;
    auto const gcode = build_gcode(doc.get(), params, ctx);
    auto const metrics = analyze_gcode_metrics(gcode, params);

    EXPECT_EQ(metrics.g0_count, 1);
    EXPECT_EQ(metrics.zero_length_g0_count, 0);
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
    auto const lines = split_lines(gcode);
    auto const first_g0 = find_first_line_with_prefix(lines, "G0 ");
    auto const first_g1 = find_first_line_with_prefix(lines, "G1 X");
    ASSERT_TRUE(first_g0.has_value());
    ASSERT_TRUE(first_g1.has_value());
    auto const g0x = parse_axis(*first_g0, 'X');
    auto const g0y = parse_axis(*first_g0, 'Y');
    auto const g1x = parse_axis(*first_g1, 'X');
    auto const g1y = parse_axis(*first_g1, 'Y');
    ASSERT_TRUE(g0x && g0y && g1x && g1y);

    EXPECT_NEAR(*g0x, 0.0, 1e-6);
    EXPECT_NEAR(*g0y, 0.0, 1e-6);
    EXPECT_GT(*g1x, 0.0);
    EXPECT_GT(*g1y, 0.0);
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
    auto const lines = split_lines(gcode);
    auto const first_g0 = find_first_line_with_prefix(lines, "G0 ");
    auto const first_g1 = find_first_line_with_prefix(lines, "G1 X");
    ASSERT_TRUE(first_g0.has_value());
    ASSERT_TRUE(first_g1.has_value());
    auto const g0x = parse_axis(*first_g0, 'X');
    auto const g0y = parse_axis(*first_g0, 'Y');
    auto const g1x = parse_axis(*first_g1, 'X');
    auto const g1y = parse_axis(*first_g1, 'Y');
    ASSERT_TRUE(g0x && g0y && g1x && g1y);

    EXPECT_NEAR(*g0x, 0.0, 1e-6);
    EXPECT_NEAR(*g1x, 15.0, 1e-6);
    EXPECT_NEAR(*g0y, *g1y, 1e-6);
}

TEST_F(GrblExportTest, ClipsLayeredGeometryToMachineBed)
{
    auto doc = create_axidraw_doc(R"A(
  <g id="layer1" inkscape:groupmode="layer" inkscape:label="Layer 1">
    <path id="p1" d="M -10,10 L 20,10" />
  </g>
  <g id="layer2" inkscape:groupmode="layer" inkscape:label="Layer 2">
    <path id="p2" d="M 5,20 L 25,20" />
  </g>
)A");
    ASSERT_TRUE(doc);
    auto *layer1 = cast<SPGroup>(doc->getObjectById("layer1"));
    auto *layer2 = cast<SPGroup>(doc->getObjectById("layer2"));
    ASSERT_TRUE(layer1);
    ASSERT_TRUE(layer2);
    layer1->setLayerMode(SPGroup::LAYER);
    layer2->setLayerMode(SPGroup::LAYER);

    Inkscape::Axidraw::GrblExportParams params;
    params.auto_pause_between_layers = true;
    params.clip_to_machine_bed = true;
    params.machine_bed_width_mm = 15.0;
    params.machine_bed_depth_mm = 50.0;

    Inkscape::Axidraw::GrblExportContext ctx;

    auto const gcode = build_gcode(doc.get(), params, ctx);

    std::size_t motion_x_count = 0;
    for (auto const &line : split_lines(gcode)) {
        if (line.rfind("G0 ", 0) != 0 && line.rfind("G1 ", 0) != 0) {
            continue;
        }
        auto const x = parse_axis(line, 'X');
        if (!x) {
            continue;
        }
        ++motion_x_count;
        EXPECT_GE(*x, 0.0) << line;
        EXPECT_LE(*x, 15.0) << line;
    }
    EXPECT_GT(motion_x_count, 0u);
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
    auto const lines = split_lines(gcode);
    auto const first_g0 = find_first_line_with_prefix(lines, "G0 ");
    auto const first_g1 = find_first_line_with_prefix(lines, "G1 X");
    ASSERT_TRUE(first_g0.has_value());
    ASSERT_TRUE(first_g1.has_value());
    auto const g0x = parse_axis(*first_g0, 'X');
    auto const g0y = parse_axis(*first_g0, 'Y');
    auto const g1x = parse_axis(*first_g1, 'X');
    auto const g1y = parse_axis(*first_g1, 'Y');
    ASSERT_TRUE(g0x && g0y && g1x && g1y);

    EXPECT_GT(*g0y, *g1y);
    EXPECT_LT(*g0x, *g1x);
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

    EXPECT_NEAR(initial[Geom::X], 37.7952755906, 1e-6);
    EXPECT_NEAR(initial[Geom::Y], 75.5905511811, 1e-6);
    EXPECT_NEAR(final[Geom::X], 113.3858267717, 1e-6);
    EXPECT_NEAR(final[Geom::Y], 151.1811023622, 1e-6);
}

TEST_F(GrblExportTest, MachinePreviewUnmapsSwappedAxes)
{
    auto doc = create_axidraw_doc(R"A(
  <path id="p1" d="M 10,20 L 30,40" />
)A");
    ASSERT_TRUE(doc);

    Inkscape::Axidraw::GrblExportParams params;
    params.swap_xy = true;

    Inkscape::Axidraw::GrblExportContext ctx;
    Geom::PathVector preview;
    std::string err;

    ASSERT_TRUE(Inkscape::Axidraw::build_grbl_plot_machine_preview_pathvector_in_doc_space(
        doc.get(), params, ctx, preview, err))
        << err;

    ASSERT_EQ(preview.size(), 1);
    auto const initial = preview.front().initialPoint();
    auto const final = preview.front().finalPoint();

    EXPECT_NEAR(initial[Geom::X], 37.7952755906, 1e-6);
    EXPECT_NEAR(initial[Geom::Y], 75.5905511811, 1e-6);
    EXPECT_NEAR(final[Geom::X], 113.3858267717, 1e-6);
    EXPECT_NEAR(final[Geom::Y], 151.1811023622, 1e-6);
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

TEST_F(GrblExportTest, MapsViewBoxUserUnitsToPhysicalMillimetres)
{
    auto const svg = std::string{
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\"\n"
        "     xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\"\n"
        "     xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\"\n"
        "     width=\"12in\"\n"
        "     height=\"16in\"\n"
        "     viewBox=\"0 0 1152 1536\">\n"
        "  <path id=\"p1\" d=\"M 403.2,288 L 460.8,288\" />\n"
        "</svg>"};
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();

    Inkscape::Axidraw::GrblExportParams params;
    Inkscape::Axidraw::GrblExportContext ctx;
    std::string err;
    std::string gcode;
    ASSERT_TRUE(Inkscape::Axidraw::build_grbl_plot_gcode_string(doc.get(), params, ctx, gcode, err)) << err;

    auto const lines = split_lines(gcode);
    auto const first_g0 = find_first_line_with_prefix(lines, "G0 ");
    auto const first_g1 = find_first_line_with_prefix(lines, "G1 X");
    ASSERT_TRUE(first_g0.has_value());
    ASSERT_TRUE(first_g1.has_value());
    std::cout << "simple_in_first_g0=" << *first_g0 << '\n';
    std::cout << "simple_in_first_g1=" << *first_g1 << '\n';

    EXPECT_EQ(first_g0->rfind("G0 X106.680 Y76.200 F6000", 0), 0) << *first_g0;
    EXPECT_EQ(first_g1->rfind("G1 X121.920 Y76.200 F1200", 0), 0) << *first_g1;
}

TEST_F(GrblExportTest, MapsTransformedIllustratorStyleContentToMillimetres)
{
    auto const svg = std::string{
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\"\n"
        "     xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\"\n"
        "     xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\"\n"
        "     width=\"12in\"\n"
        "     height=\"16in\"\n"
        "     viewBox=\"0 0 1152 1536\">\n"
        "  <g transform=\"matrix(1.3319636,0,0,1.331819,0.59653895,0.58191415)\">\n"
        "    <line x1=\"403.2\" y1=\"288\" x2=\"460.8\" y2=\"288\" style=\"stroke:#000;stroke-width:1;fill:none\" />\n"
        "  </g>\n"
        "</svg>"};
    auto doc = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();

    Inkscape::Axidraw::GrblExportParams params;
    Inkscape::Axidraw::GrblExportContext ctx;
    std::string err;
    std::string gcode;
    ASSERT_TRUE(Inkscape::Axidraw::build_grbl_plot_gcode_string(doc.get(), params, ctx, gcode, err)) << err;

    auto const lines = split_lines(gcode);
    auto const first_g0 = find_first_line_with_prefix(lines, "G0 ");
    auto const first_g1 = find_first_line_with_prefix(lines, "G1 X");
    ASSERT_TRUE(first_g0.has_value());
    ASSERT_TRUE(first_g1.has_value());
    std::cout << "transform_in_first_g0=" << *first_g0 << '\n';
    std::cout << "transform_in_first_g1=" << *first_g1 << '\n';

    EXPECT_EQ(first_g0->rfind("G0 X142.252 Y101.639 F6000", 0), 0) << *first_g0;
    EXPECT_EQ(first_g1->rfind("G1 X162.551 Y101.639 F1200", 0), 0) << *first_g1;
}

TEST_F(GrblExportTest, HeadlessRealWorldSvgSmoke)
{
    auto const *svg_path_c = std::getenv("INKSCAPE_GRBL_HEADLESS_SVG");
    if (!svg_path_c || !*svg_path_c) {
        GTEST_SKIP() << "Set INKSCAPE_GRBL_HEADLESS_SVG to run the real-world headless GRBL export smoke test.";
    }

    std::string const svg_path = svg_path_c;
    auto const svg_text = read_text_file(svg_path);
    ASSERT_FALSE(svg_text.empty()) << "Cannot read SVG: " << svg_path;

    auto doc = SPDocument::createNewDoc(svg_path.c_str());
    ASSERT_TRUE(doc) << "Cannot load document: " << svg_path;
    doc->ensureUpToDate();

    Inkscape::Axidraw::GrblExportParams params;
    Inkscape::Axidraw::GrblExportContext ctx;
    Inkscape::Axidraw::GrblPlotStats stats;
    std::string err;

    auto const t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(Inkscape::Axidraw::analyze_grbl_plot(doc.get(), params, ctx, stats, err)) << err;
    auto const t1 = std::chrono::steady_clock::now();

    std::size_t strokes = 0;
    std::string gcode;
    ASSERT_TRUE(Inkscape::Axidraw::build_grbl_plot_gcode_string(doc.get(), params, ctx, gcode, err, &strokes,
                                                                256u * 1024u * 1024u, &stats))
        << err;
    auto const t2 = std::chrono::steady_clock::now();

    auto const metrics = analyze_gcode_metrics(gcode, params);
    auto const raw_layer_count = count_raw_layers(svg_text);

    std::string output_path;
    if (auto const *output_c = std::getenv("INKSCAPE_GRBL_HEADLESS_GCODE")) {
        output_path = output_c;
    } else {
        auto const dot = svg_path.find_last_of('.');
        output_path = (dot == std::string::npos ? svg_path : svg_path.substr(0, dot)) + ".headless.gcode";
    }
    ASSERT_TRUE(write_text_file(output_path, gcode)) << "Cannot write G-code: " << output_path;

    auto const analyze_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    auto const build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    auto const dims_px = doc->getDimensions();
    auto const viewbox = doc->getViewBox();

    std::ostringstream report;
    report << "\n[HEADLESS_GRBL_REPORT]\n";
    report << "svg=" << svg_path << '\n';
    report << "gcode=" << output_path << '\n';
    report << std::fixed << std::setprecision(3)
           << "doc_dims_px=[" << dims_px[Geom::X] << "," << dims_px[Geom::Y] << "] "
           << "viewbox=[" << viewbox.left() << "," << viewbox.top() << "]-[" << viewbox.right() << ","
           << viewbox.bottom() << "]\n";
    report << "raw_layer_count=" << raw_layer_count << " core_layer_count=" << stats.layer_count
           << " stroke_count=" << strokes << '\n';
    if (stats.has_bounds_mm) {
        report << std::fixed << std::setprecision(3)
               << "bounds_mm=[" << stats.min_x_mm << "," << stats.min_y_mm << "]-[" << stats.max_x_mm << ","
               << stats.max_y_mm << "]\n";
    }
    if (stats.has_length_stats) {
        report << std::fixed << std::setprecision(3)
               << "draw_length_mm=" << stats.draw_length_mm
               << " travel_length_mm=" << stats.travel_length_mm
               << " estimated_duration_sec=" << stats.estimated_duration_sec << '\n';
    }
    report << "analyze_ms=" << analyze_ms << " build_ms=" << build_ms << '\n';
    report << "gcode_lines_total=" << metrics.total_lines
           << " nonempty=" << metrics.nonempty_lines
           << " G0=" << metrics.g0_count
           << " G1=" << metrics.g1_count
           << " M3=" << metrics.m3_count
           << " M5=" << metrics.m5_count
           << " M6=" << metrics.m6_count
           << " G4=" << metrics.g4_count << '\n';
    report << "pen_up_like=" << metrics.pen_up_like_count
           << " pen_down_like=" << metrics.pen_down_like_count
           << " repeated_motion_target=" << metrics.repeated_motion_target_count
           << " zero_length_g0=" << metrics.zero_length_g0_count
           << " zero_length_g1=" << metrics.zero_length_g1_count << '\n';
    report << std::fixed << std::setprecision(6)
           << "min_draw_segment_mm=" << metrics.min_draw_segment_mm
           << " max_draw_segment_mm=" << metrics.max_draw_segment_mm
           << " min_travel_segment_mm=" << metrics.min_travel_segment_mm
           << " max_travel_segment_mm=" << metrics.max_travel_segment_mm << '\n';
    std::cout << report.str();
    ASSERT_TRUE(write_text_file(output_path + ".report.txt", report.str()));

    EXPECT_FALSE(gcode.empty());
    EXPECT_EQ(strokes, stats.stroke_count);
    EXPECT_EQ(split_lines(gcode).front(), "G21");
    EXPECT_NE(gcode.find("\nG90\n"), std::string::npos);
    EXPECT_GT(metrics.g0_count, 0);
    EXPECT_GT(metrics.g1_count, 0);
    EXPECT_EQ(metrics.zero_length_g1_count, 0);
}
