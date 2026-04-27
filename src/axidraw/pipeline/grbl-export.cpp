// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Export visible shape geometry as GRBL G-code over an open serial port.
 */

#include "grbl-export.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <list>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <2geom/path.h>
#include <2geom/pathvector.h>
#include <2geom/point.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/serial-port.h"
#include "desktop.h"
#include "document.h"
#include "selection.h"
#include "helper/geom-curves.h"
#include "helper/geom.h"
#include "object/sp-defs.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "object/sp-object.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "layer-manager.h"
#include "util/cast.h"
#include "util/units.h"
#include "preferences.h"

#include <glibmm/i18n.h>

namespace Inkscape::Axidraw {

namespace {

using Inkscape::Util::Quantity;
using Inkscape::Util::Unit;
using Inkscape::Util::UnitTable;

constexpr double k_mm_per_in = 25.4;
constexpr double k_px_per_in = 96.0;
constexpr double k_mm_per_px = k_mm_per_in / k_px_per_in;
constexpr double k_machine_coord_epsilon_mm = 1e-3;
constexpr auto k_default_pen_up_gcode = "G90\nG1 Z0 F3000";
constexpr auto k_default_pen_down_gcode = "G90\nG1 Z5 F3000";
constexpr auto k_legacy_pen_up_gcode = "G91\nG1 Z-5 F10000\nG90";
constexpr auto k_legacy_pen_down_gcode = "G91\nG1 Z5 F10000\nG90";
constexpr auto k_default_end_gcode = "G0 X0 Y0";
constexpr auto k_pref_clip_bed = "/options/grbl/clip-to-machine-bed";
constexpr auto k_pref_clip_bed_migration_v1 = "/options/grbl/migrations/clip-to-machine-bed-default-v1";
constexpr auto k_pref_end_gcode = "/options/grbl/end-gcode";
constexpr auto k_pref_end_gcode_migration_v1 = "/options/grbl/migrations/end-gcode-default-v1";
constexpr auto k_pref_pen_z_absolute_migration_v1 = "/options/grbl/migrations/pen-z-absolute-default-v1";

void migrate_clip_to_machine_bed_default(Inkscape::Preferences *prefs)
{
    if (!prefs || prefs->getEntry(k_pref_clip_bed_migration_v1).isSet()) {
        return;
    }

    prefs->setBool(k_pref_clip_bed, true);
    prefs->setBool(k_pref_clip_bed_migration_v1, true);
    prefs->save();
}

void migrate_end_gcode_default(Inkscape::Preferences *prefs)
{
    if (!prefs || prefs->getEntry(k_pref_end_gcode_migration_v1).isSet()) {
        return;
    }

    auto end_gcode = prefs->getString(k_pref_end_gcode);
    if (end_gcode.find_first_not_of(" \t\r\n") == std::string::npos) {
        prefs->setString(k_pref_end_gcode, k_default_end_gcode);
    }
    prefs->setBool(k_pref_end_gcode_migration_v1, true);
    prefs->save();
}

void migrate_pen_z_absolute_default(Inkscape::Preferences *prefs)
{
    if (!prefs || prefs->getEntry(k_pref_pen_z_absolute_migration_v1).isSet()) {
        return;
    }

    constexpr auto k_pref_pen_up = "/options/grbl/pen-up-cmd";
    constexpr auto k_pref_pen_down = "/options/grbl/pen-down-cmd";

    auto const pen_up = prefs->getString(k_pref_pen_up);
    auto const pen_down = prefs->getString(k_pref_pen_down);
    if (pen_up == k_legacy_pen_up_gcode && pen_down == k_legacy_pen_down_gcode) {
        prefs->setString(k_pref_pen_up, k_default_pen_up_gcode);
        prefs->setString(k_pref_pen_down, k_default_pen_down_gcode);
    }
    prefs->setBool(k_pref_pen_z_absolute_migration_v1, true);
    prefs->save();
}

std::string normalize_end_gcode(std::string value)
{
    return value;
}

bool emit_optional_dwell_ms(std::function<bool(std::string const &)> const &emit_line, double const delay_ms)
{
    if (!(delay_ms > 1e-9)) {
        return true;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "G4 P%.3f", delay_ms / 1000.0);
    return emit_line(buf);
}

bool parse_svg_length_to_mm(char const *text, double &mm_out)
{
    if (!text) {
        return false;
    }

    std::string value(text);
    auto const first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return false;
    }
    auto const last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);

    char *end = nullptr;
    double const number = std::strtod(value.c_str(), &end);
    if (!end || end == value.c_str()) {
        return false;
    }

    std::string unit(end);
    auto const unit_first = unit.find_first_not_of(" \t\r\n");
    if (unit_first == std::string::npos) {
        unit.clear();
    } else {
        auto const unit_last = unit.find_last_not_of(" \t\r\n");
        unit = unit.substr(unit_first, unit_last - unit_first + 1);
    }
    for (auto &ch : unit) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    double scale = 0.0;
    if (unit.empty() || unit == "px") {
        scale = k_mm_per_px;
    } else if (unit == "mm") {
        scale = 1.0;
    } else if (unit == "cm") {
        scale = 10.0;
    } else if (unit == "in") {
        scale = k_mm_per_in;
    } else if (unit == "pt") {
        scale = k_mm_per_in / 72.0;
    } else if (unit == "pc") {
        scale = k_mm_per_in / 6.0;
    } else if (unit == "q") {
        scale = 0.25;
    } else {
        return false;
    }

    mm_out = number * scale;
    return true;
}

bool get_document_physical_size_mm(SPDocument *doc, double &width_mm, double &height_mm)
{
    width_mm = 0.0;
    height_mm = 0.0;
    if (!doc) {
        return false;
    }

    if (auto *root = doc->getRoot()) {
        double raw_width_mm = 0.0;
        double raw_height_mm = 0.0;
        if (parse_svg_length_to_mm(root->getAttribute("width"), raw_width_mm) &&
            parse_svg_length_to_mm(root->getAttribute("height"), raw_height_mm) &&
            raw_width_mm > 1e-9 && raw_height_mm > 1e-9) {
            width_mm = raw_width_mm;
            height_mm = raw_height_mm;
            return true;
        }

        if (root->width.computed > 1e-9 && root->height.computed > 1e-9) {
            width_mm = root->width.computed * k_mm_per_px;
            height_mm = root->height.computed * k_mm_per_px;
            return true;
        }
    }

    auto const page_px = doc->getDimensions();
    width_mm = page_px[Geom::X] * k_mm_per_px;
    height_mm = page_px[Geom::Y] * k_mm_per_px;
    return width_mm > 1e-9 && height_mm > 1e-9;
}

struct DocumentMmMapper {
    double source_origin_x_doc = 0.0;
    double source_origin_y_doc = 0.0;
    double mm_per_source_doc_x = 0.0;
    double mm_per_source_doc_y = 0.0;
    double preview_doc_units_per_mm_x = 1.0 / k_mm_per_px;
    double preview_doc_units_per_mm_y = 1.0 / k_mm_per_px;

    [[nodiscard]] bool valid() const
    {
        return mm_per_source_doc_x > 0.0 && mm_per_source_doc_y > 0.0;
    }

    [[nodiscard]] Geom::Point doc_to_mm(Geom::Point const &p) const
    {
        return {(p[Geom::X] - source_origin_x_doc) * mm_per_source_doc_x,
                (p[Geom::Y] - source_origin_y_doc) * mm_per_source_doc_y};
    }

    [[nodiscard]] Geom::Point mm_to_doc(Geom::Point const &p) const
    {
        return {p[Geom::X] * preview_doc_units_per_mm_x,
                p[Geom::Y] * preview_doc_units_per_mm_y};
    }

    [[nodiscard]] double avg_doc_units_per_mm() const
    {
        return 0.5 * ((1.0 / mm_per_source_doc_x) + (1.0 / mm_per_source_doc_y));
    }
};

DocumentMmMapper build_document_mm_mapper(SPDocument *doc)
{
    DocumentMmMapper mapper;
    if (!doc) {
        return mapper;
    }

    auto const viewbox = doc->getViewBox();
    double page_w_mm = 0.0;
    double page_h_mm = 0.0;
    if (!get_document_physical_size_mm(doc, page_w_mm, page_h_mm)) {
        return mapper;
    }

    if (viewbox.width() > 1e-9 && page_w_mm > 1e-9) {
        mapper.source_origin_x_doc = viewbox.left();
        mapper.mm_per_source_doc_x = page_w_mm / viewbox.width();
    }
    if (viewbox.height() > 1e-9 && page_h_mm > 1e-9) {
        mapper.source_origin_y_doc = viewbox.top();
        mapper.mm_per_source_doc_y = page_h_mm / viewbox.height();
    }

    if (!mapper.valid()) {
        double const px_to_mm = k_mm_per_px;
        mapper.source_origin_x_doc = 0.0;
        mapper.source_origin_y_doc = 0.0;
        mapper.mm_per_source_doc_x = px_to_mm;
        mapper.mm_per_source_doc_y = px_to_mm;
    }

    return mapper;
}

void append_stroke_from_path(Geom::Path const &pit, std::vector<std::vector<Geom::Point>> &strokes_doc)
{
    std::vector<Geom::Point> stroke;
    for (Geom::Path::const_iterator cit = pit.begin(); cit != pit.end_default(); ++cit) {
        if (!is_straight_curve(*cit)) {
            continue;
        }
        if (stroke.empty()) {
            stroke.push_back(cit->initialPoint());
        }
        stroke.push_back(cit->finalPoint());
    }
    if (stroke.size() >= 2) {
        // Remove duplicate consecutive points.
        std::vector<Geom::Point> dedup;
        dedup.reserve(stroke.size());
        for (auto const &pt : stroke) {
            if (dedup.empty() || !Geom::are_near(dedup.back(), pt, 1e-9)) {
                dedup.push_back(pt);
            }
        }
        if (dedup.size() >= 2) {
            strokes_doc.push_back(std::move(dedup));
        }
    }
}

Geom::Affine get_missing_root_viewbox_correction(SPDocument *doc)
{
    if (!doc) {
        return Geom::identity();
    }

    auto *root = doc->getRoot();
    if (!root || !root->viewBox_set || root->viewBox.width() <= 1e-9 || root->viewBox.height() <= 1e-9) {
        return Geom::identity();
    }

    double page_w_mm = 0.0;
    double page_h_mm = 0.0;
    if (!get_document_physical_size_mm(doc, page_w_mm, page_h_mm)) {
        return Geom::identity();
    }
    auto const scale = doc->getDocumentScale(true);
    bool const needs_viewbox_transform =
        std::abs(scale[Geom::X] - 1.0) > 1e-9 || std::abs(scale[Geom::Y] - 1.0) > 1e-9 ||
        std::abs(root->viewBox.left()) > 1e-9 || std::abs(root->viewBox.top()) > 1e-9;
    if (!needs_viewbox_transform) {
        return Geom::identity();
    }

    auto const &c2p = root->c2p;
    bool const root_c2p_is_identity =
        std::abs(c2p[0] - 1.0) <= 1e-9 && std::abs(c2p[1]) <= 1e-9 &&
        std::abs(c2p[2]) <= 1e-9 && std::abs(c2p[3] - 1.0) <= 1e-9 &&
        std::abs(c2p[4]) <= 1e-9 && std::abs(c2p[5]) <= 1e-9;
    if (!root_c2p_is_identity) {
        return Geom::identity();
    }

    return Geom::Scale(scale[Geom::X], scale[Geom::Y]) *
           Geom::Translate(-root->viewBox.left(), -root->viewBox.top());
}

void collect_shapes_recursive(SPObject *obj, double flatness, std::vector<std::vector<Geom::Point>> &strokes_doc)
{
    if (!obj || is<SPDefs>(obj)) {
        return;
    }

    if (auto *item = cast<SPItem>(obj)) {
        if (item->isHidden()) {
            return;
        }
    }

    if (auto *shape = cast<SPShape>(obj)) {
        if (auto const *curve = shape->curve()) {
            auto *item = cast<SPItem>(shape);
            if (!item) {
                return;
            }
            Geom::Affine const tf = item->i2doc_affine() * get_missing_root_viewbox_correction(shape->document);
            Geom::PathVector const pv = (*curve) * tf;
            Geom::PathVector const linear = pathv_to_linear(pv, flatness);
            for (auto const &pit : linear) {
                append_stroke_from_path(pit, strokes_doc);
            }
        }
    }

    for (auto *child : obj->childList(false)) {
        collect_shapes_recursive(child, flatness, strokes_doc);
    }
}

static bool stroke_is_closed_for_reorder(std::vector<Geom::Point> const &stroke)
{
    return stroke.size() >= 4 && Geom::are_near(stroke.front(), stroke.back(), 1e-9);
}

static Geom::Point extend_point_along_segment(Geom::Point const &anchor, Geom::Point const &other, double const distance_mm)
{
    auto const delta = anchor - other;
    double const length = Geom::L2(delta);
    if (!(length > 1e-9) || !(distance_mm > 1e-9)) {
        return anchor;
    }
    return anchor + (delta / length) * distance_mm;
}

static void apply_open_stroke_lead_in_out(std::vector<std::vector<Geom::Point>> &strokes,
                                          double const lead_in_distance_mm,
                                          double const lead_out_distance_mm)
{
    if (!(lead_in_distance_mm > 1e-9) && !(lead_out_distance_mm > 1e-9)) {
        return;
    }
    for (auto &stroke : strokes) {
        if (stroke.size() < 2 || stroke_is_closed_for_reorder(stroke)) {
            continue;
        }
        if (lead_in_distance_mm > 1e-9) {
            stroke.front() = extend_point_along_segment(stroke.front(), stroke[1], lead_in_distance_mm);
        }
        if (lead_out_distance_mm > 1e-9) {
            stroke.back() = extend_point_along_segment(stroke.back(), stroke[stroke.size() - 2], lead_out_distance_mm);
        }
    }
}

static void apply_open_stroke_lead_in_out_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers,
                                                 double const lead_in_distance_mm,
                                                 double const lead_out_distance_mm)
{
    for (auto &layer : layers) {
        apply_open_stroke_lead_in_out(layer, lead_in_distance_mm, lead_out_distance_mm);
    }
}

static void rotate_closed_stroke_start(std::vector<Geom::Point> &stroke, std::size_t start_index)
{
    if (!stroke_is_closed_for_reorder(stroke)) {
        return;
    }

    auto const unique_count = stroke.size() - 1;
    if (unique_count < 3 || start_index == 0 || start_index >= unique_count) {
        return;
    }

    std::vector<Geom::Point> rotated;
    rotated.reserve(stroke.size());
    rotated.insert(rotated.end(), stroke.begin() + start_index, stroke.begin() + unique_count);
    rotated.insert(rotated.end(), stroke.begin(), stroke.begin() + start_index);
    rotated.push_back(rotated.front());
    stroke = std::move(rotated);
}

static void rotate_closed_stroke_start_near(std::vector<Geom::Point> &stroke, Geom::Point const &target)
{
    if (!stroke_is_closed_for_reorder(stroke)) {
        return;
    }

    auto const unique_count = stroke.size() - 1;
    std::size_t best_index = 0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < unique_count; ++i) {
        double const d = Geom::L2(stroke[i] - target);
        if (d < best_distance) {
            best_distance = d;
            best_index = i;
        }
    }

    rotate_closed_stroke_start(stroke, best_index);
}

static void rotate_closed_stroke_start_between(std::vector<Geom::Point> &stroke, Geom::Point const &prev_end,
                                               Geom::Point const *next_start)
{
    if (!stroke_is_closed_for_reorder(stroke)) {
        return;
    }

    auto const unique_count = stroke.size() - 1;
    std::size_t best_index = 0;
    double best_cost = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < unique_count; ++i) {
        double cost = Geom::L2(stroke[i] - prev_end);
        if (next_start) {
            cost += Geom::L2(stroke[i] - *next_start);
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_index = i;
        }
    }

    rotate_closed_stroke_start(stroke, best_index);
}

static void reorder_strokes_nearest_neighbor(std::vector<std::vector<Geom::Point>> &strokes, bool allow_reverse)
{
    strokes.erase(std::remove_if(strokes.begin(), strokes.end(),
                                 [](std::vector<Geom::Point> const &s) { return s.size() < 2; }),
                  strokes.end());
    if (strokes.size() <= 1) {
        return;
    }
    Geom::Point const origin(0, 0);
    std::vector<std::vector<Geom::Point>> out;
    out.reserve(strokes.size());
    std::vector<bool> used(strokes.size(), false);
    Geom::Point pos = origin;

    for (size_t n = 0; n < strokes.size(); ++n) {
        size_t best = 0;
        double best_d = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < strokes.size(); ++i) {
            if (used[i] || strokes[i].empty()) {
                continue;
            }
            double best_i = Geom::L2(strokes[i].front() - pos);
            if (best_i < best_d) {
                best_d = best_i;
                best = i;
            }
            if (allow_reverse && best_i > 0 && strokes[i].size() >= 2) {
                double const d_back = Geom::L2(strokes[i].back() - pos);
                if (d_back < best_d) {
                    best_d = d_back;
                    best = i;
                }
            }
        }
        used[best] = true;
        if (allow_reverse && !strokes[best].empty() &&
            Geom::L2(strokes[best].back() - pos) < Geom::L2(strokes[best].front() - pos)) {
            std::reverse(strokes[best].begin(), strokes[best].end());
        }
        rotate_closed_stroke_start_near(strokes[best], pos);
        out.push_back(std::move(strokes[best]));
        if (!out.back().empty()) {
            pos = out.back().back();
        }
    }
    strokes = std::move(out);

    auto const dist = [](Geom::Point const &a, Geom::Point const &b) { return Geom::L2(a - b); };
    auto const stroke_start = [](std::vector<Geom::Point> const &s, bool rev) -> Geom::Point {
        return rev ? s.back() : s.front();
    };
    auto const stroke_end = [](std::vector<Geom::Point> const &s, bool rev) -> Geom::Point {
        return rev ? s.front() : s.back();
    };

    // Local orientation pass: with neighbors fixed, flip individual strokes if it reduces air moves.
    if (allow_reverse && strokes.size() > 1) {
        for (int pass = 0; pass < 3; ++pass) {
            bool changed = false;
            for (std::size_t i = 0; i < strokes.size(); ++i) {
                auto &cur = strokes[i];
                if (cur.size() < 2) {
                    continue;
                }
                Geom::Point const prev_end = (i == 0) ? origin : strokes[i - 1].back();
                bool const has_next = (i + 1) < strokes.size();
                Geom::Point const next_start = has_next ? strokes[i + 1].front() : cur.back();

                rotate_closed_stroke_start_between(cur, prev_end, has_next ? &next_start : nullptr);

                double const keep_cost = dist(prev_end, cur.front()) + (has_next ? dist(cur.back(), next_start) : 0.0);
                double const rev_cost = dist(prev_end, cur.back()) + (has_next ? dist(cur.front(), next_start) : 0.0);
                if (rev_cost + 1e-9 < keep_cost) {
                    std::reverse(cur.begin(), cur.end());
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
    }

    // 2-opt-lite on adjacent pairs (with optional endpoint reversal), tuned for speed on large jobs.
    if (strokes.size() > 2) {
        for (int iter = 0; iter < 3; ++iter) {
            bool any_change = false;
            for (std::size_t i = 0; i + 1 < strokes.size(); ++i) {
                auto &a = strokes[i];
                auto &b = strokes[i + 1];
                if (a.size() < 2 || b.size() < 2) {
                    continue;
                }
                Geom::Point const prev_end = (i == 0) ? origin : strokes[i - 1].back();
                bool const has_next = (i + 2) < strokes.size();
                Geom::Point const next_start = has_next ? strokes[i + 2].front() : b.back();

                struct Choice {
                    bool swap = false;
                    bool rev_a = false;
                    bool rev_b = false;
                    double cost = std::numeric_limits<double>::infinity();
                };
                Choice best;

                for (int swapped = 0; swapped <= 1; ++swapped) {
                    for (int ra = 0; ra <= (allow_reverse ? 1 : 0); ++ra) {
                        for (int rb = 0; rb <= (allow_reverse ? 1 : 0); ++rb) {
                            auto const &first = swapped ? b : a;
                            auto const &second = swapped ? a : b;
                            bool const first_rev = swapped ? (rb != 0) : (ra != 0);
                            bool const second_rev = swapped ? (ra != 0) : (rb != 0);

                            Geom::Point const first_s = stroke_start(first, first_rev);
                            Geom::Point const first_e = stroke_end(first, first_rev);
                            Geom::Point const second_s = stroke_start(second, second_rev);
                            Geom::Point const second_e = stroke_end(second, second_rev);
                            double const c = dist(prev_end, first_s) + dist(first_e, second_s) +
                                             (has_next ? dist(second_e, next_start) : 0.0);
                            if (c + 1e-9 < best.cost) {
                                best = Choice{swapped != 0, ra != 0, rb != 0, c};
                            }
                        }
                    }
                }

                bool changed = false;
                if (best.rev_a) {
                    std::reverse(a.begin(), a.end());
                    changed = true;
                }
                if (best.rev_b) {
                    std::reverse(b.begin(), b.end());
                    changed = true;
                }
                if (best.swap) {
                    std::swap(strokes[i], strokes[i + 1]);
                    changed = true;
                }
                any_change = any_change || changed;
            }
            if (!any_change) {
                break;
            }
        }
    }

    for (std::size_t i = 0; i < strokes.size(); ++i) {
        auto &cur = strokes[i];
        if (cur.size() < 2) {
            continue;
        }
        Geom::Point const prev_end = (i == 0) ? origin : strokes[i - 1].back();
        bool const has_next = (i + 1) < strokes.size();
        Geom::Point const next_start = has_next ? strokes[i + 1].front() : cur.back();
        rotate_closed_stroke_start_between(cur, prev_end, has_next ? &next_start : nullptr);
    }
}

static void reorder_strokes_layers_nearest_neighbor(std::vector<std::vector<std::vector<Geom::Point>>> &layers,
                                                    bool allow_reverse)
{
    for (auto &layer : layers) {
        if (layer.size() > 1) {
            reorder_strokes_nearest_neighbor(layer, allow_reverse);
        }
    }
}

static Geom::Point quantize_point_for_grbl(Geom::Point const &p, double quantum_mm = 0.001)
{
    if (!(quantum_mm > 0.0)) {
        return p;
    }

    auto const snap = [quantum_mm](double value) {
        return std::round(value / quantum_mm) * quantum_mm;
    };
    return {snap(p[Geom::X]), snap(p[Geom::Y])};
}

static void reorder_strokes_quantized_for_grbl(std::vector<std::vector<Geom::Point>> &strokes, bool allow_reverse,
                                               double quantum_mm = 0.001)
{
    strokes.erase(std::remove_if(strokes.begin(), strokes.end(),
                                 [](std::vector<Geom::Point> const &s) { return s.size() < 2; }),
                  strokes.end());
    if (strokes.size() <= 1) {
        return;
    }

    struct StrokeRef {
        std::vector<Geom::Point> points;
        Geom::Point front_q;
        Geom::Point back_q;
        bool closed = false;
    };

    std::vector<StrokeRef> refs;
    refs.reserve(strokes.size());
    for (auto &stroke : strokes) {
        auto const front_q = quantize_point_for_grbl(stroke.front(), quantum_mm);
        auto const back_q = quantize_point_for_grbl(stroke.back(), quantum_mm);
        bool const closed = stroke_is_closed_for_reorder(stroke);
        refs.push_back(StrokeRef{std::move(stroke), front_q, back_q, closed});
    }

    Geom::Point const origin(0, 0);
    Geom::Point pos = origin;
    Geom::Point pos_q = quantize_point_for_grbl(origin, quantum_mm);
    std::vector<std::vector<Geom::Point>> out;
    out.reserve(refs.size());
    std::vector<bool> used(refs.size(), false);

    for (std::size_t n = 0; n < refs.size(); ++n) {
        std::size_t best = 0;
        double best_d = std::numeric_limits<double>::infinity();
        bool reverse_best = false;

        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (used[i] || refs[i].points.size() < 2) {
                continue;
            }

            double d_front = Geom::L2(refs[i].front_q - pos_q);
            if (d_front < best_d) {
                best_d = d_front;
                best = i;
                reverse_best = false;
            }

            if (allow_reverse && !refs[i].closed) {
                double const d_back = Geom::L2(refs[i].back_q - pos_q);
                if (d_back < best_d) {
                    best_d = d_back;
                    best = i;
                    reverse_best = true;
                }
            }
        }

        used[best] = true;
        auto &chosen = refs[best];
        if (reverse_best) {
            std::reverse(chosen.points.begin(), chosen.points.end());
            std::swap(chosen.front_q, chosen.back_q);
        }
        if (chosen.closed) {
            rotate_closed_stroke_start_near(chosen.points, pos);
            chosen.front_q = quantize_point_for_grbl(chosen.points.front(), quantum_mm);
            chosen.back_q = quantize_point_for_grbl(chosen.points.back(), quantum_mm);
        }

        out.push_back(std::move(chosen.points));
        pos = out.back().back();
        pos_q = quantize_point_for_grbl(pos, quantum_mm);
    }

    strokes = std::move(out);
}

static void reorder_strokes_layers_quantized_for_grbl(std::vector<std::vector<std::vector<Geom::Point>>> &layers,
                                                      bool allow_reverse, double quantum_mm = 0.001)
{
    for (auto &layer : layers) {
        if (layer.size() > 1) {
            reorder_strokes_quantized_for_grbl(layer, allow_reverse, quantum_mm);
        }
    }
}

static bool try_orient_adjacent_strokes_for_exact_join(std::vector<Geom::Point> &a, std::vector<Geom::Point> &b,
                                                       double endpoint_eps)
{
    if (a.size() < 2 || b.size() < 2) {
        return false;
    }

    if (Geom::L2(a.back() - b.front()) <= endpoint_eps) {
        return true;
    }
    if (Geom::L2(a.back() - b.back()) <= endpoint_eps) {
        std::reverse(b.begin(), b.end());
        return true;
    }
    if (Geom::L2(a.front() - b.front()) <= endpoint_eps) {
        std::reverse(a.begin(), a.end());
        return true;
    }
    if (Geom::L2(a.front() - b.back()) <= endpoint_eps) {
        std::reverse(a.begin(), a.end());
        std::reverse(b.begin(), b.end());
        return true;
    }
    return false;
}

static void merge_exact_touching_strokes(std::vector<std::vector<Geom::Point>> &strokes, double endpoint_eps = 0.01)
{
    if (strokes.size() < 2 || endpoint_eps <= 0.0) {
        return;
    }

    struct StrokeRef {
        std::size_t original_index = 0;
        std::vector<Geom::Point> points;
    };

    auto append_oriented = [endpoint_eps](std::vector<Geom::Point> &dst, std::vector<Geom::Point> const &src,
                                          std::size_t start_index = 0) {
        if (src.size() < 2) {
            return;
        }

        auto append_point = [&](Geom::Point const &pt) {
            if (dst.empty() || Geom::L2(dst.back() - pt) > endpoint_eps) {
                dst.push_back(pt);
            }
        };

        for (std::size_t i = start_index; i < src.size(); ++i) {
            append_point(src[i]);
        }
    };

    std::vector<StrokeRef> open_strokes;
    std::vector<std::pair<std::size_t, std::vector<Geom::Point>>> preserved;
    open_strokes.reserve(strokes.size());
    preserved.reserve(strokes.size());
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        auto &stroke = strokes[i];
        if (stroke.size() < 2) {
            continue;
        }
        if (stroke_is_closed_for_reorder(stroke)) {
            preserved.emplace_back(i, std::move(stroke));
            continue;
        }
        open_strokes.push_back(StrokeRef{i, std::move(stroke)});
    }

    if (open_strokes.size() < 2) {
        std::vector<std::pair<std::size_t, std::vector<Geom::Point>>> rebuilt;
        rebuilt.reserve(open_strokes.size() + preserved.size());
        for (auto &stroke : open_strokes) {
            rebuilt.emplace_back(stroke.original_index, std::move(stroke.points));
        }
        rebuilt.insert(rebuilt.end(), std::make_move_iterator(preserved.begin()), std::make_move_iterator(preserved.end()));
        std::stable_sort(rebuilt.begin(), rebuilt.end(), [](auto const &a, auto const &b) {
            return a.first < b.first;
        });

        std::vector<std::vector<Geom::Point>> out;
        out.reserve(rebuilt.size());
        for (auto &entry : rebuilt) {
            if (entry.second.size() >= 2) {
                out.push_back(std::move(entry.second));
            }
        }
        strokes = std::move(out);
        return;
    }

    std::vector<StrokeRef> merged_open_strokes;
    merged_open_strokes.reserve(open_strokes.size());
    for (std::size_t i = 0; i < open_strokes.size(); ++i) {
        auto merged_points = std::move(open_strokes[i].points);
        auto merged_index = open_strokes[i].original_index;

        while (i + 1 < open_strokes.size()) {
            auto next_points = std::move(open_strokes[i + 1].points);
            if (!try_orient_adjacent_strokes_for_exact_join(merged_points, next_points, endpoint_eps)) {
                open_strokes[i + 1].points = std::move(next_points);
                break;
            }

            append_oriented(merged_points, next_points, 1);
            merged_index = std::min(merged_index, open_strokes[i + 1].original_index);
            ++i;
        }

        merged_open_strokes.push_back(StrokeRef{merged_index, std::move(merged_points)});
    }

    std::vector<std::pair<std::size_t, std::vector<Geom::Point>>> rebuilt;
    rebuilt.reserve(merged_open_strokes.size() + preserved.size());
    for (auto &stroke : merged_open_strokes) {
        rebuilt.emplace_back(stroke.original_index, std::move(stroke.points));
    }
    rebuilt.insert(rebuilt.end(), std::make_move_iterator(preserved.begin()), std::make_move_iterator(preserved.end()));
    std::stable_sort(rebuilt.begin(), rebuilt.end(), [](auto const &a, auto const &b) {
        return a.first < b.first;
    });

    std::vector<std::vector<Geom::Point>> out;
    out.reserve(rebuilt.size());
    for (auto &entry : rebuilt) {
        if (entry.second.size() >= 2) {
            out.push_back(std::move(entry.second));
        }
    }
    strokes = std::move(out);
}

static void merge_exact_touching_strokes_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers,
                                                double endpoint_eps = 0.01)
{
    for (auto &layer : layers) {
        merge_exact_touching_strokes(layer, endpoint_eps);
    }
}

static bool stroke_is_closed(std::vector<Geom::Point> const &stroke, double eps_mm = 0.05)
{
    return stroke.size() >= 4 && Geom::L2(stroke.front() - stroke.back()) <= eps_mm;
}

static Geom::Point rotate_point_around(Geom::Point const &point, Geom::Point const &origin, double const angle_rad)
{
    double const s = std::sin(angle_rad);
    double const c = std::cos(angle_rad);
    double const dx = point[Geom::X] - origin[Geom::X];
    double const dy = point[Geom::Y] - origin[Geom::Y];
    return {origin[Geom::X] + dx * c - dy * s, origin[Geom::Y] + dx * s + dy * c};
}

struct HatchConversionStats
{
    std::size_t closed_contour_candidates = 0;
    std::size_t converted_contours = 0;
    std::size_t inset_applied_contours = 0;
    std::size_t inset_fallback_contours = 0;
};

static double signed_polygon_area(std::vector<Geom::Point> const &poly)
{
    if (poly.size() < 3) {
        return 0.0;
    }

    double twice_area = 0.0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        auto const &a = poly[i];
        auto const &b = poly[(i + 1) % poly.size()];
        twice_area += a[Geom::X] * b[Geom::Y] - b[Geom::X] * a[Geom::Y];
    }
    return twice_area * 0.5;
}

static double cross2d(Geom::Point const &a, Geom::Point const &b)
{
    return a[Geom::X] * b[Geom::Y] - a[Geom::Y] * b[Geom::X];
}

static bool line_intersection(Geom::Point const &a0, Geom::Point const &a1, Geom::Point const &b0,
                              Geom::Point const &b1, Geom::Point &out)
{
    auto const da = a1 - a0;
    auto const db = b1 - b0;
    double const denom = cross2d(da, db);
    if (std::abs(denom) <= 1e-9) {
        return false;
    }

    double const t = cross2d(b0 - a0, db) / denom;
    out = a0 + da * t;
    return std::isfinite(out[Geom::X]) && std::isfinite(out[Geom::Y]);
}

static bool inset_closed_stroke(std::vector<Geom::Point> const &closed_stroke, double inset_distance,
                                std::vector<Geom::Point> &inset_stroke)
{
    inset_stroke.clear();
    if (closed_stroke.size() < 4 || inset_distance <= 1e-9) {
        return false;
    }

    std::vector<Geom::Point> poly = closed_stroke;
    if (Geom::L2(poly.front() - poly.back()) <= 1e-9) {
        poly.pop_back();
    }
    if (poly.size() < 3) {
        return false;
    }

    double const area = signed_polygon_area(poly);
    double const abs_area = std::abs(area);
    if (abs_area <= 1e-9) {
        return false;
    }

    double const normal_sign = area >= 0.0 ? 1.0 : -1.0;
    inset_stroke.reserve(poly.size() + 1);

    for (std::size_t i = 0; i < poly.size(); ++i) {
        auto const &prev = poly[(i + poly.size() - 1) % poly.size()];
        auto const &curr = poly[i];
        auto const &next = poly[(i + 1) % poly.size()];

        auto const edge0 = curr - prev;
        auto const edge1 = next - curr;
        double const len0 = Geom::L2(edge0);
        double const len1 = Geom::L2(edge1);
        if (len0 <= 1e-9 || len1 <= 1e-9) {
            return false;
        }

        Geom::Point const normal0((-edge0[Geom::Y] / len0) * normal_sign, (edge0[Geom::X] / len0) * normal_sign);
        Geom::Point const normal1((-edge1[Geom::Y] / len1) * normal_sign, (edge1[Geom::X] / len1) * normal_sign);
        Geom::Point vertex;
        if (!line_intersection(prev + normal0 * inset_distance, curr + normal0 * inset_distance,
                               curr + normal1 * inset_distance, next + normal1 * inset_distance, vertex)) {
            return false;
        }
        inset_stroke.push_back(vertex);
    }

    if (inset_stroke.size() < 3) {
        inset_stroke.clear();
        return false;
    }

    double const inset_area = std::abs(signed_polygon_area(inset_stroke));
    if (!(inset_area > 1e-9) || inset_area >= abs_area) {
        inset_stroke.clear();
        return false;
    }

    for (std::size_t i = 0; i < inset_stroke.size(); ++i) {
        auto const &a = inset_stroke[i];
        auto const &b = inset_stroke[(i + 1) % inset_stroke.size()];
        if (!std::isfinite(a[Geom::X]) || !std::isfinite(a[Geom::Y]) || Geom::L2(b - a) <= 1e-6) {
            inset_stroke.clear();
            return false;
        }
    }

    inset_stroke.push_back(inset_stroke.front());
    return true;
}

static std::vector<std::vector<Geom::Point>>
contour_to_hatch_scanlines(std::vector<Geom::Point> const &closed_stroke, double spacing_mm, double angle_deg)
{
    std::vector<std::vector<Geom::Point>> out;
    if (closed_stroke.size() < 4 || spacing_mm <= 1e-6) {
        return out;
    }

    std::vector<Geom::Point> poly = closed_stroke;
    if (Geom::L2(poly.front() - poly.back()) <= 1e-9) {
        poly.pop_back();
    }
    if (poly.size() < 3) {
        return out;
    }

    double min_x = std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();
    for (auto const &p : poly) {
        min_x = std::min(min_x, p[Geom::X]);
        min_y = std::min(min_y, p[Geom::Y]);
        max_x = std::max(max_x, p[Geom::X]);
        max_y = std::max(max_y, p[Geom::Y]);
    }
    if (!std::isfinite(min_x) || !std::isfinite(min_y) || !std::isfinite(max_x) || !std::isfinite(max_y) ||
        max_y - min_y < 1e-6) {
        return out;
    }

    Geom::Point const rotation_center((min_x + max_x) * 0.5, (min_y + max_y) * 0.5);
    double const angle_rad = angle_deg * M_PI / 180.0;
    if (std::abs(angle_rad) > 1e-9) {
        for (auto &p : poly) {
            p = rotate_point_around(p, rotation_center, -angle_rad);
        }
        min_y = std::numeric_limits<double>::infinity();
        max_y = -std::numeric_limits<double>::infinity();
        for (auto const &p : poly) {
            min_y = std::min(min_y, p[Geom::Y]);
            max_y = std::max(max_y, p[Geom::Y]);
        }
        if (!std::isfinite(min_y) || !std::isfinite(max_y) || max_y - min_y < 1e-6) {
            return out;
        }
    }

    bool flip_dir = false;
    for (double y = min_y + spacing_mm * 0.5; y <= max_y - spacing_mm * 0.5 + 1e-9; y += spacing_mm) {
        std::vector<double> xs;
        xs.reserve(poly.size());
        for (std::size_t i = 0; i < poly.size(); ++i) {
            Geom::Point const &a = poly[i];
            Geom::Point const &b = poly[(i + 1) % poly.size()];
            double const y0 = a[Geom::Y];
            double const y1 = b[Geom::Y];
            if (std::abs(y1 - y0) <= 1e-12) {
                continue;
            }
            double const y_min = std::min(y0, y1);
            double const y_max = std::max(y0, y1);
            if (y < y_min || y >= y_max) {
                continue;
            }
            double const t = (y - y0) / (y1 - y0);
            xs.push_back(a[Geom::X] + (b[Geom::X] - a[Geom::X]) * t);
        }
        if (xs.size() < 2) {
            continue;
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
            double x0 = xs[k];
            double x1 = xs[k + 1];
            if (x1 - x0 < 1e-6) {
                continue;
            }
            std::vector<Geom::Point> seg;
            seg.reserve(2);
            if (flip_dir) {
                seg.emplace_back(x1, y);
                seg.emplace_back(x0, y);
            } else {
                seg.emplace_back(x0, y);
                seg.emplace_back(x1, y);
            }
            if (std::abs(angle_rad) > 1e-9) {
                for (auto &point : seg) {
                    point = rotate_point_around(point, rotation_center, angle_rad);
                }
            }
            out.push_back(std::move(seg));
            flip_dir = !flip_dir;
        }
    }
    return out;
}

static void convert_closed_contours_to_hatch(std::vector<std::vector<Geom::Point>> &strokes_mm, double spacing_mm,
                                             double angle_deg, bool cross_hatch, bool inset_hatch,
                                             double inset_distance, bool angle_increment_enable,
                                             double angle_increment_deg, HatchConversionStats *stats = nullptr)
{
    if (spacing_mm <= 1e-6 || strokes_mm.empty()) {
        return;
    }
    std::vector<std::vector<Geom::Point>> out;
    out.reserve(strokes_mm.size());
    double current_angle_deg = angle_deg;
    for (auto const &stroke : strokes_mm) {
        if (!stroke_is_closed(stroke)) {
            out.push_back(stroke);
            continue;
        }
        if (stats) {
            ++stats->closed_contour_candidates;
        }
        std::vector<Geom::Point> hatch_source;
        bool const inset_applied = inset_hatch && inset_closed_stroke(stroke, inset_distance, hatch_source);
        if (stats && inset_hatch) {
            if (inset_applied) {
                ++stats->inset_applied_contours;
            } else {
                ++stats->inset_fallback_contours;
            }
        }
        auto const &source = inset_applied ? hatch_source : stroke;
        auto hatch = contour_to_hatch_scanlines(source, spacing_mm, current_angle_deg);
        if (cross_hatch) {
            auto cross = contour_to_hatch_scanlines(source, spacing_mm, current_angle_deg + 90.0);
            hatch.insert(hatch.end(), std::make_move_iterator(cross.begin()), std::make_move_iterator(cross.end()));
        }
        if (angle_increment_enable) {
            current_angle_deg += angle_increment_deg;
        }
        if (hatch.empty()) {
            out.push_back(stroke);
            continue;
        }
        if (stats) {
            ++stats->converted_contours;
        }
        out.insert(out.end(), std::make_move_iterator(hatch.begin()), std::make_move_iterator(hatch.end()));
    }
    strokes_mm = std::move(out);
}

static void convert_closed_contours_to_hatch_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers_mm,
                                                    double spacing_mm, double angle_deg, bool cross_hatch,
                                                    bool inset_hatch, double inset_distance,
                                                    bool angle_increment_enable, double angle_increment_deg,
                                                    HatchConversionStats *stats = nullptr)
{
    if (spacing_mm <= 1e-6 || layers_mm.empty()) {
        return;
    }
    double current_angle_deg = angle_deg;
    for (auto &layer : layers_mm) {
        if (layer.empty()) {
            continue;
        }
        std::vector<std::vector<Geom::Point>> out;
        out.reserve(layer.size());
        for (auto const &stroke : layer) {
            if (!stroke_is_closed(stroke)) {
                out.push_back(stroke);
                continue;
            }
            if (stats) {
                ++stats->closed_contour_candidates;
            }
            std::vector<Geom::Point> hatch_source;
            bool const inset_applied = inset_hatch && inset_closed_stroke(stroke, inset_distance, hatch_source);
            if (stats && inset_hatch) {
                if (inset_applied) {
                    ++stats->inset_applied_contours;
                } else {
                    ++stats->inset_fallback_contours;
                }
            }
            auto const &source = inset_applied ? hatch_source : stroke;
            auto hatch = contour_to_hatch_scanlines(source, spacing_mm, current_angle_deg);
            if (cross_hatch) {
                auto cross = contour_to_hatch_scanlines(source, spacing_mm, current_angle_deg + 90.0);
                hatch.insert(hatch.end(), std::make_move_iterator(cross.begin()), std::make_move_iterator(cross.end()));
            }
            if (angle_increment_enable) {
                current_angle_deg += angle_increment_deg;
            }
            if (hatch.empty()) {
                out.push_back(stroke);
                continue;
            }
            if (stats) {
                ++stats->converted_contours;
            }
            out.insert(out.end(), std::make_move_iterator(hatch.begin()), std::make_move_iterator(hatch.end()));
        }
        layer = std::move(out);
    }
}

void collect_strokes_for_context(GrblExportContext const &ctx, SPDocument *doc, double const flatness,
                                 std::vector<std::vector<Geom::Point>> &strokes_doc)
{
    if (ctx.cancel && ctx.cancel->load()) {
        return;
    }
    if (ctx.selection && !ctx.selection->isEmpty()) {
        for (SPItem *item : ctx.selection->items_vector()) {
            if (item) {
                collect_shapes_recursive(item, flatness, strokes_doc);
            }
            if (ctx.cancel && ctx.cancel->load()) {
                return;
            }
        }
        return;
    }
    if (ctx.use_current_layer_without_selection && ctx.desktop) {
        if (auto *layer = ctx.desktop->layerManager().currentLayer()) {
            collect_shapes_recursive(layer, flatness, strokes_doc);
            return;
        }
    }
    collect_shapes_recursive(doc->getRoot(), flatness, strokes_doc);
}

std::string format_xy_mm(Geom::Point const &p_mm, char const *cmd, double feed)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << std::setprecision(3) << cmd << " X" << p_mm[Geom::X] << " Y" << p_mm[Geom::Y] << " F" << feed;
    return oss.str();
}

static double document_page_height_mm(SPDocument *doc)
{
    double width_mm = 0.0;
    double height_mm = 0.0;
    if (!get_document_physical_size_mm(doc, width_mm, height_mm)) {
        return 0.0;
    }
    return height_mm;
}

static void strokes_doc_to_mm(std::vector<std::vector<Geom::Point>> const &strokes_doc, DocumentMmMapper const &mapper,
                              std::vector<std::vector<Geom::Point>> &strokes_mm)
{
    strokes_mm.clear();
    strokes_mm.reserve(strokes_doc.size());
    for (auto const &st : strokes_doc) {
        if (st.size() < 2) {
            continue;
        }
        std::vector<Geom::Point> mm;
        mm.reserve(st.size());
        for (auto const &p : st) {
            mm.push_back(mapper.doc_to_mm(p));
        }
        strokes_mm.push_back(std::move(mm));
    }
}

static void apply_flip_y_canvas(std::vector<std::vector<Geom::Point>> &strokes, double page_h_mm)
{
    if (!(page_h_mm > 0)) {
        return;
    }
    for (auto &st : strokes) {
        for (auto &p : st) {
            p[Geom::Y] = page_h_mm - p[Geom::Y];
        }
    }
}

static void apply_axis_mapping(std::vector<std::vector<Geom::Point>> &strokes, bool swap_xy, bool invert_x, bool invert_y,
                               double machine_bed_width_mm, double machine_bed_depth_mm)
{
    if (!swap_xy && !invert_x && !invert_y) {
        return;
    }
    for (auto &st : strokes) {
        for (auto &p : st) {
            if (swap_xy) {
                std::swap(p[Geom::X], p[Geom::Y]);
            }
            auto const mapped_bed_width_mm = swap_xy ? machine_bed_depth_mm : machine_bed_width_mm;
            auto const mapped_bed_depth_mm = swap_xy ? machine_bed_width_mm : machine_bed_depth_mm;
            if (invert_x) {
                p[Geom::X] = mapped_bed_width_mm > 1e-6 ? (mapped_bed_width_mm - p[Geom::X]) : -p[Geom::X];
            }
            if (invert_y) {
                p[Geom::Y] = mapped_bed_depth_mm > 1e-6 ? (mapped_bed_depth_mm - p[Geom::Y]) : -p[Geom::Y];
            }
        }
    }
}

static std::pair<double, double> get_mapped_machine_bed_dims(bool swap_xy, double machine_bed_width_mm,
                                                             double machine_bed_depth_mm)
{
    if (swap_xy) {
        return {machine_bed_depth_mm, machine_bed_width_mm};
    }
    return {machine_bed_width_mm, machine_bed_depth_mm};
}

/** @param shift_x_out / shift_y_out when non-null, receive the minima subtracted (mm). */
static bool apply_align_min_to_origin(std::vector<std::vector<Geom::Point>> &strokes, double *shift_x_out,
                                      double *shift_y_out)
{
    double minx = std::numeric_limits<double>::infinity();
    double miny = std::numeric_limits<double>::infinity();
    for (auto const &st : strokes) {
        for (auto const &p : st) {
            minx = std::min(minx, p[Geom::X]);
            miny = std::min(miny, p[Geom::Y]);
        }
    }
    if (!std::isfinite(minx) || !std::isfinite(miny)) {
        if (shift_x_out) {
            *shift_x_out = 0;
        }
        if (shift_y_out) {
            *shift_y_out = 0;
        }
        return false;
    }
    if (shift_x_out) {
        *shift_x_out = minx;
    }
    if (shift_y_out) {
        *shift_y_out = miny;
    }
    for (auto &st : strokes) {
        for (auto &p : st) {
            p[Geom::X] -= minx;
            p[Geom::Y] -= miny;
        }
    }
    return true;
}

static bool liang_barsky_clip(double x0, double y0, double x1, double y1, double xmin, double ymin, double xmax,
                              double ymax, double &ox0, double &oy0, double &ox1, double &oy1)
{
    double t0 = 0;
    double t1 = 1;
    double const dx = x1 - x0;
    double const dy = y1 - y0;
    auto const edge = [&](double p, double q) -> bool {
        if (std::abs(p) < 1e-30) {
            return q >= 0;
        }
        double const r = q / p;
        if (p < 0) {
            if (r > t1) {
                return false;
            }
            if (r > t0) {
                t0 = r;
            }
        } else {
            if (r < t0) {
                return false;
            }
            if (r < t1) {
                t1 = r;
            }
        }
        return true;
    };
    if (!edge(-dx, x0 - xmin)) {
        return false;
    }
    if (!edge(dx, xmax - x0)) {
        return false;
    }
    if (!edge(-dy, y0 - ymin)) {
        return false;
    }
    if (!edge(dy, ymax - y0)) {
        return false;
    }
    if (t0 > t1) {
        return false;
    }
    ox0 = x0 + t0 * dx;
    oy0 = y0 + t0 * dy;
    ox1 = x0 + t1 * dx;
    oy1 = y0 + t1 * dy;
    return true;
}

static constexpr double k_clip_eps_mm = 5e-4;

static void clip_strokes_to_axis_rect(std::vector<std::vector<Geom::Point>> &strokes, double xmin, double ymin,
                                      double xmax, double ymax)
{
    if (!(xmax > xmin && ymax > ymin)) {
        return;
    }

    std::vector<std::vector<Geom::Point>> out;
    out.reserve(strokes.size() * 2);

    for (auto const &st : strokes) {
        if (st.size() < 2) {
            continue;
        }
        std::vector<Geom::Point> current;

        auto flush = [&] {
            if (current.size() < 2) {
                current.clear();
                return;
            }
            std::vector<Geom::Point> dedup;
            dedup.reserve(current.size());
            for (auto const &p : current) {
                if (dedup.empty() || !Geom::are_near(dedup.back(), p, k_clip_eps_mm)) {
                    dedup.push_back(p);
                }
            }
            if (dedup.size() >= 2) {
                out.push_back(std::move(dedup));
            }
            current.clear();
        };

        for (size_t i = 0; i + 1 < st.size(); ++i) {
            double const x0 = st[i][Geom::X];
            double const y0 = st[i][Geom::Y];
            double const x1 = st[i + 1][Geom::X];
            double const y1 = st[i + 1][Geom::Y];
            double ax = 0;
            double ay = 0;
            double bx = 0;
            double by = 0;
            if (!liang_barsky_clip(x0, y0, x1, y1, xmin, ymin, xmax, ymax, ax, ay, bx, by)) {
                flush();
                continue;
            }
            Geom::Point const A(ax, ay);
            Geom::Point const B(bx, by);
            if (Geom::are_near(A, B, k_clip_eps_mm)) {
                continue;
            }

            if (current.empty()) {
                current.push_back(A);
                current.push_back(B);
            } else if (Geom::are_near(current.back(), A, k_clip_eps_mm)) {
                current.push_back(B);
            } else {
                flush();
                current.push_back(A);
                current.push_back(B);
            }
        }
        flush();
    }

    strokes = std::move(out);
}

static void connect_nearby_strokes(std::vector<std::vector<Geom::Point>> &strokes, double near_dist_mm)
{
    if (near_dist_mm <= 1e-9 || strokes.size() < 2) {
        return;
    }

    struct EndpointRef {
        std::size_t stroke_index = 0;
        bool at_front = true;
        Geom::Point point;
    };

    struct CandidateEdge {
        int endpoint_a = -1;
        int endpoint_b = -1;
        double gap = 0.0;
    };

    struct Chain {
        bool active = true;
        std::vector<std::size_t> member_indices;
        std::vector<std::vector<Geom::Point>> pieces;
        int front_endpoint = -1;
        int back_endpoint = -1;
    };

    auto reverse_chain = [](Chain &chain) {
        std::reverse(chain.pieces.begin(), chain.pieces.end());
        for (auto &piece : chain.pieces) {
            std::reverse(piece.begin(), piece.end());
        }
        std::swap(chain.front_endpoint, chain.back_endpoint);
    };

    auto append_piece = [](std::vector<std::vector<Geom::Point>> &dst, std::vector<Geom::Point> src) {
        if (src.size() < 2) {
            return;
        }
        if (!dst.empty() && !dst.back().empty() && Geom::are_near(dst.back().back(), src.front(), 1e-9)) {
            src.erase(src.begin());
        }
        if (src.size() >= 2) {
            dst.push_back(std::move(src));
        }
    };

    std::vector<std::vector<Geom::Point>> filtered;
    filtered.reserve(strokes.size());
    for (auto &stroke : strokes) {
        if (stroke.size() >= 2) {
            filtered.push_back(std::move(stroke));
        }
    }
    if (filtered.size() < 2) {
        strokes = std::move(filtered);
        return;
    }

    std::vector<EndpointRef> endpoints;
    endpoints.reserve(filtered.size() * 2);
    for (std::size_t i = 0; i < filtered.size(); ++i) {
        endpoints.push_back(EndpointRef{i, true, filtered[i].front()});
        endpoints.push_back(EndpointRef{i, false, filtered[i].back()});
    }

    auto const endpoint_id = [](std::size_t stroke_index, bool at_front) {
        return static_cast<int>(stroke_index * 2 + (at_front ? 0 : 1));
    };

    double const cell_size = near_dist_mm;
    auto const cell_key = [cell_size](Geom::Point const &pt) {
        long long const gx = static_cast<long long>(std::floor(pt[Geom::X] / cell_size));
        long long const gy = static_cast<long long>(std::floor(pt[Geom::Y] / cell_size));
        return std::pair<long long, long long>(gx, gy);
    };

    struct PairHash {
        std::size_t operator()(std::pair<long long, long long> const &p) const
        {
            auto const hx = std::hash<long long>{}(p.first);
            auto const hy = std::hash<long long>{}(p.second);
            return hx ^ (hy + 0x9e3779b97f4a7c15ULL + (hx << 6) + (hx >> 2));
        }
    };

    std::unordered_map<std::pair<long long, long long>, std::vector<int>, PairHash> grid;
    grid.reserve(endpoints.size() * 2);
    for (int id = 0; id < static_cast<int>(endpoints.size()); ++id) {
        grid[cell_key(endpoints[id].point)].push_back(id);
    }

    std::vector<CandidateEdge> edges;
    for (int a = 0; a < static_cast<int>(endpoints.size()); ++a) {
        auto const [gx, gy] = cell_key(endpoints[a].point);
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                auto it = grid.find({gx + dx, gy + dy});
                if (it == grid.end()) {
                    continue;
                }
                for (int b : it->second) {
                    if (b <= a) {
                        continue;
                    }
                    if (endpoints[a].stroke_index == endpoints[b].stroke_index) {
                        continue;
                    }
                    double const gap = Geom::L2(endpoints[a].point - endpoints[b].point);
                    if (gap <= near_dist_mm) {
                        edges.push_back(CandidateEdge{a, b, gap});
                    }
                }
            }
        }
    }

    if (edges.empty()) {
        strokes = std::move(filtered);
        return;
    }

    std::stable_sort(edges.begin(), edges.end(), [](CandidateEdge const &a, CandidateEdge const &b) {
        return a.gap < b.gap;
    });

    std::vector<Chain> chains(filtered.size());
    std::vector<std::size_t> stroke_to_chain(filtered.size());
    for (std::size_t i = 0; i < filtered.size(); ++i) {
        chains[i].member_indices.push_back(i);
        chains[i].pieces.push_back(std::move(filtered[i]));
        chains[i].front_endpoint = endpoint_id(i, true);
        chains[i].back_endpoint = endpoint_id(i, false);
        stroke_to_chain[i] = i;
    }

    for (auto const &edge : edges) {
        auto const &end_a = endpoints[edge.endpoint_a];
        auto const &end_b = endpoints[edge.endpoint_b];
        std::size_t const chain_a_id = stroke_to_chain[end_a.stroke_index];
        std::size_t const chain_b_id = stroke_to_chain[end_b.stroke_index];
        if (chain_a_id == chain_b_id) {
            continue;
        }

        auto &chain_a = chains[chain_a_id];
        auto &chain_b = chains[chain_b_id];
        if (!chain_a.active || !chain_b.active) {
            continue;
        }

        bool const a_is_exposed = edge.endpoint_a == chain_a.front_endpoint || edge.endpoint_a == chain_a.back_endpoint;
        bool const b_is_exposed = edge.endpoint_b == chain_b.front_endpoint || edge.endpoint_b == chain_b.back_endpoint;
        if (!a_is_exposed || !b_is_exposed) {
            continue;
        }

        if (edge.endpoint_a == chain_a.front_endpoint) {
            reverse_chain(chain_a);
        }
        if (edge.endpoint_b == chain_b.back_endpoint) {
            reverse_chain(chain_b);
        }

        if (edge.endpoint_a != chain_a.back_endpoint || edge.endpoint_b != chain_b.front_endpoint) {
            continue;
        }

        for (auto &piece : chain_b.pieces) {
            append_piece(chain_a.pieces, std::move(piece));
        }
        chain_a.back_endpoint = chain_b.back_endpoint;

        for (auto member : chain_b.member_indices) {
            stroke_to_chain[member] = chain_a_id;
            chain_a.member_indices.push_back(member);
        }

        chain_b.active = false;
        chain_b.member_indices.clear();
        chain_b.pieces.clear();
    }

    std::vector<std::pair<std::size_t, std::vector<std::vector<Geom::Point>>>> merged;
    merged.reserve(chains.size());
    for (auto &chain : chains) {
        if (!chain.active || chain.pieces.empty()) {
            continue;
        }
        auto const first_member = *std::min_element(chain.member_indices.begin(), chain.member_indices.end());
        merged.emplace_back(first_member, std::move(chain.pieces));
    }

    std::stable_sort(merged.begin(), merged.end(), [](auto const &a, auto const &b) {
        return a.first < b.first;
    });

    std::vector<std::vector<Geom::Point>> out;
    out.reserve(strokes.size());
    for (auto &entry : merged) {
        for (auto &piece : entry.second) {
            if (piece.size() >= 2) {
                out.push_back(std::move(piece));
            }
        }
    }

    strokes = std::move(out);
}

static void connect_nearby_strokes_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers, double near_dist_mm)
{
    if (near_dist_mm <= 1e-9) {
        return;
    }
    for (auto &layer : layers) {
        connect_nearby_strokes(layer, near_dist_mm);
    }
}

static void sparse_sample_strokes_legacy(std::vector<std::vector<Geom::Point>> &strokes, int keep_every)
{
    if (keep_every <= 1 || strokes.size() <= 1) {
        return;
    }

    std::vector<std::vector<Geom::Point>> out;
    out.reserve((strokes.size() + keep_every - 1) / keep_every);
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        if ((i % static_cast<std::size_t>(keep_every)) == 0) {
            out.push_back(std::move(strokes[i]));
        }
    }
    strokes = std::move(out);
}

static void sparse_sample_strokes_directional(std::vector<std::vector<Geom::Point>> &strokes, int keep_every)
{
    if (keep_every <= 1 || strokes.size() <= 1) {
        return;
    }

    struct SparseCandidate {
        std::size_t index = 0;
        int angle_bin = 0;
        double offset = 0.0;
        double along = 0.0;
    };

    constexpr double k_straight_ratio_min = 0.995;
    constexpr double k_angle_bin_deg = 10.0;

    auto const stroke_length = [](std::vector<Geom::Point> const &stroke) {
        double len = 0.0;
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            len += Geom::L2(stroke[i] - stroke[i - 1]);
        }
        return len;
    };

    std::vector<bool> keep(strokes.size(), true);
    std::vector<SparseCandidate> candidates;
    candidates.reserve(strokes.size());

    for (std::size_t i = 0; i < strokes.size(); ++i) {
        auto const &stroke = strokes[i];
        if (stroke.size() < 2) {
            continue;
        }

        auto const chord = stroke.back() - stroke.front();
        double const chord_len = Geom::L2(chord);
        double const draw_len = stroke_length(stroke);
        if (!(draw_len > 1e-9) || !(chord_len > 1e-9) || (chord_len / draw_len) < k_straight_ratio_min) {
            continue;
        }

        double angle = std::atan2(chord[Geom::Y], chord[Geom::X]);
        if (angle < 0.0) {
            angle += M_PI;
        }
        if (angle >= M_PI) {
            angle -= M_PI;
        }
        int const angle_bin = static_cast<int>(std::floor((angle * 180.0 / M_PI) / k_angle_bin_deg));
        double const dir_x = std::cos(angle);
        double const dir_y = std::sin(angle);
        double const normal_x = -dir_y;
        double const normal_y = dir_x;
        auto const mid = (stroke.front() + stroke.back()) * 0.5;

        keep[i] = false;
        candidates.push_back(SparseCandidate{i, angle_bin, mid[Geom::X] * normal_x + mid[Geom::Y] * normal_y,
                                             mid[Geom::X] * dir_x + mid[Geom::Y] * dir_y});
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](SparseCandidate const &a, SparseCandidate const &b) {
        if (a.angle_bin != b.angle_bin) {
            return a.angle_bin < b.angle_bin;
        }
        if (std::abs(a.offset - b.offset) > 1e-9) {
            return a.offset < b.offset;
        }
        if (std::abs(a.along - b.along) > 1e-9) {
            return a.along < b.along;
        }
        return a.index < b.index;
    });

    int current_bin = std::numeric_limits<int>::min();
    std::size_t rank_in_bin = 0;
    for (auto const &candidate : candidates) {
        if (candidate.angle_bin != current_bin) {
            current_bin = candidate.angle_bin;
            rank_in_bin = 0;
        }
        if ((rank_in_bin % static_cast<std::size_t>(keep_every)) == 0) {
            keep[candidate.index] = true;
        }
        ++rank_in_bin;
    }

    std::vector<std::vector<Geom::Point>> out;
    out.reserve((strokes.size() + keep_every - 1) / keep_every);
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        if (keep[i]) {
            out.push_back(std::move(strokes[i]));
        }
    }
    strokes = std::move(out);
}

static void sparse_sample_strokes(std::vector<std::vector<Geom::Point>> &strokes, int keep_every,
                                  SparseSamplingStrategy strategy)
{
    switch (strategy) {
        case SparseSamplingStrategy::Legacy:
            sparse_sample_strokes_legacy(strokes, keep_every);
            return;
        case SparseSamplingStrategy::Directional:
            sparse_sample_strokes_directional(strokes, keep_every);
            return;
    }
}

static void sparse_sample_strokes_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers, int keep_every,
                                         SparseSamplingStrategy strategy)
{
    if (keep_every <= 1) {
        return;
    }
    for (auto &layer : layers) {
        sparse_sample_strokes(layer, keep_every, strategy);
    }
}

static std::size_t count_custom_gcode_lines(Glib::ustring const &block)
{
    auto for_each_gcode_block_line = [](std::string block, auto &&fn) {
        for (auto &ch : block) {
            if (ch == '\r' || ch == '\0') {
                ch = '\n';
            }
        }

        std::istringstream in(block);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            auto pos = line.find_first_not_of(" \t");
            if (pos == std::string::npos) {
                continue;
            }
            line.erase(0, pos);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (line.empty() || line[0] == ';') {
                continue;
            }
            if (line[0] == '(') {
                auto const end = line.find(')');
                if (end != std::string::npos && end + 1 == line.size()) {
                    continue;
                }
            }
            fn(line);
        }
    };

    std::size_t count = 0;
    for_each_gcode_block_line(block.raw(), [&](std::string const &) {
        ++count;
    });
    return count;
}

template <typename Fn>
static bool for_each_trimmed_gcode_line(std::string block, Fn &&fn)
{
    for (auto &ch : block) {
        if (ch == '\r' || ch == '\0') {
            ch = '\n';
        }
    }

    std::istringstream in(block);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos) {
            continue;
        }
        line.erase(0, pos);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == ';') {
            continue;
        }
        if (line[0] == '(') {
            auto const end = line.find(')');
            if (end != std::string::npos && end + 1 == line.size()) {
                continue;
            }
        }
        if (!fn(line)) {
            return false;
        }
    }
    return true;
}

} // namespace

struct PreparedPlotMm {
    bool layered = false;
    std::vector<std::vector<Geom::Point>> flat_mm;
    std::vector<std::vector<std::vector<Geom::Point>>> layers_mm;
    std::vector<int> layer_tool_ids;
    std::vector<Glib::ustring> layer_labels;
    HatchConversionStats hatch_stats;

    /// Set by fill_prepared_plot_mm for inverse “machine mm → document” canvas preview.
    bool preview_flip_y_applied = false;
    double preview_page_h_mm = 0;
    bool preview_align_applied = false;
    double preview_align_shift_x_mm = 0;
    double preview_align_shift_y_mm = 0;
    bool preview_swap_xy_applied = false;
    bool preview_invert_x_applied = false;
    bool preview_invert_y_applied = false;
    bool preview_clip_applied = false;
    double preview_machine_bed_width_mm = 0;
    double preview_machine_bed_depth_mm = 0;
    bool has_travel_optimization_stats = false;
    double travel_length_before_optimization_mm = 0;

    std::size_t count_strokes() const
    {
        if (!layered) {
            return flat_mm.size();
        }
        std::size_t n = 0;
        for (auto const &layer : layers_mm) {
            n += layer.size();
        }
        return n;
    }
};

static double stroke_draw_length_mm(std::vector<Geom::Point> const &stroke)
{
    if (stroke.size() < 2) {
        return 0.0;
    }
    double len = 0.0;
    for (std::size_t i = 1; i < stroke.size(); ++i) {
        len += Geom::L2(stroke[i] - stroke[i - 1]);
    }
    return len;
}

static bool compute_grbl_plot_lengths_from_prep(PreparedPlotMm const &prep, GrblExportParams const &params,
                                                double &draw_length_mm, double &travel_length_mm)
{
    draw_length_mm = 0.0;
    travel_length_mm = 0.0;

    Geom::Point pos(0, 0);
    bool has_any = false;

    auto const consume_layer = [&](std::vector<std::vector<Geom::Point>> const &layer,
                                   double &draw_out, double &travel_out, Geom::Point &pos_inout, bool &has_any_inout) {
        for (auto const &stroke : layer) {
            if (stroke.size() < 2) {
                continue;
            }
            has_any_inout = true;
            travel_out += Geom::L2(stroke.front() - pos_inout);
            draw_out += stroke_draw_length_mm(stroke);
            pos_inout = stroke.back();
        }
    };

    if (!prep.layered) {
        consume_layer(prep.flat_mm, draw_length_mm, travel_length_mm, pos, has_any);
    } else {
        bool first_nonempty_layer = true;
        int active_tool = -1;
        for (std::size_t li = 0; li < prep.layers_mm.size(); ++li) {
            auto const &layer = prep.layers_mm[li];
            bool const layer_has_stroke = std::any_of(layer.begin(), layer.end(),
                                                      [](auto const &stroke) { return stroke.size() >= 2; });
            if (!layer_has_stroke) {
                continue;
            }
            int const next_tool = li < prep.layer_tool_ids.size() ? prep.layer_tool_ids[li] : -1;
            bool const needs_tool_change =
                !first_nonempty_layer && params.enable_layer_tool_change_m6 && next_tool >= 0 && next_tool != active_tool;
            if (!first_nonempty_layer && params.auto_pause_between_layers && params.manual_pen_change &&
                params.pen_change_to_home) {
                Geom::Point const origin(0, 0);
                Geom::Point const resume = pos;
                travel_length_mm += Geom::L2(origin - resume);
                travel_length_mm += Geom::L2(resume - origin);
            } else if (needs_tool_change && params.tool_change_use_point) {
                Geom::Point const change(params.tool_change_x_mm, params.tool_change_y_mm);
                Geom::Point const resume = pos;
                travel_length_mm += Geom::L2(change - resume);
                travel_length_mm += Geom::L2(resume - change);
            }
            consume_layer(layer, draw_length_mm, travel_length_mm, pos, has_any);
            first_nonempty_layer = false;
            if (next_tool >= 0) {
                active_tool = next_tool;
            }
        }
    }

    return has_any;
}

static void fill_grbl_plot_lengths_from_prep(PreparedPlotMm const &prep, GrblExportParams const &params,
                                             GrblPlotStats &st)
{
    st.has_length_stats = compute_grbl_plot_lengths_from_prep(prep, params, st.draw_length_mm, st.travel_length_mm);
    st.has_travel_optimization_stats = false;
    st.travel_length_before_optimization_mm = 0.0;
    st.travel_length_saved_by_optimization_mm = 0.0;
    if (prep.has_travel_optimization_stats) {
        st.has_travel_optimization_stats = true;
        st.travel_length_before_optimization_mm = prep.travel_length_before_optimization_mm;
        st.travel_length_saved_by_optimization_mm =
            std::max(0.0, prep.travel_length_before_optimization_mm - st.travel_length_mm);
    }
}

static void fill_grbl_plot_stats_from_prep(PreparedPlotMm const &prep, GrblExportParams const &params,
                                           GrblPlotStats &st)
{
    st.stroke_count = prep.count_strokes();
    st.layer_count = prep.layered ? prep.layers_mm.size() : (prep.flat_mm.empty() ? 0 : 1);
    st.tool_change_count = 0;
    st.contour_to_hatch_requested = params.contour_to_hatch;
    st.hatch_inset_requested = params.contour_to_hatch && params.hatch_inset_enable;
    st.hatch_closed_contour_candidates = prep.hatch_stats.closed_contour_candidates;
    st.hatch_converted_contours = prep.hatch_stats.converted_contours;
    st.hatch_inset_applied_contours = prep.hatch_stats.inset_applied_contours;
    st.hatch_inset_fallback_contours = prep.hatch_stats.inset_fallback_contours;
    st.has_bounds_mm = false;
    double minx = std::numeric_limits<double>::infinity();
    double miny = std::numeric_limits<double>::infinity();
    double maxx = -std::numeric_limits<double>::infinity();
    double maxy = -std::numeric_limits<double>::infinity();
    auto consider = [&](Geom::Point const &p) {
        minx = std::min(minx, p[Geom::X]);
        miny = std::min(miny, p[Geom::Y]);
        maxx = std::max(maxx, p[Geom::X]);
        maxy = std::max(maxy, p[Geom::Y]);
    };
    if (prep.layered) {
        for (auto const &layer : prep.layers_mm) {
            for (auto const &stroke : layer) {
                for (auto const &p : stroke) {
                    consider(p);
                }
            }
        }
    } else {
        for (auto const &stroke : prep.flat_mm) {
            for (auto const &p : stroke) {
                consider(p);
            }
        }
    }
    if (std::isfinite(minx) && std::isfinite(miny) && std::isfinite(maxx) && std::isfinite(maxy) &&
        st.stroke_count > 0) {
        st.has_bounds_mm = true;
        st.min_x_mm = minx;
        st.min_y_mm = miny;
        st.max_x_mm = maxx;
        st.max_y_mm = maxy;
    }
    fill_grbl_plot_lengths_from_prep(prep, params, st);
    if (prep.layered && prep.layers_mm.size() > 1 && params.enable_layer_tool_change_m6) {
        int active_tool = -1;
        for (std::size_t li = 0; li < prep.layers_mm.size(); ++li) {
            int const next_tool = li < prep.layer_tool_ids.size() ? prep.layer_tool_ids[li] : -1;
            bool const needs_tool_change =
                li > 0 && next_tool >= 0 && next_tool != active_tool;
            if (needs_tool_change) {
                ++st.tool_change_count;
            }
            if (next_tool >= 0) {
                active_tool = next_tool;
            }
        }
    }
    double duration_sec = 0.0;
    if (params.feed_draw_mm_min > 1e-9) {
        duration_sec += (st.draw_length_mm / params.feed_draw_mm_min) * 60.0;
    }
    if (params.feed_travel_mm_min > 1e-9) {
        duration_sec += (st.travel_length_mm / params.feed_travel_mm_min) * 60.0;
    }
    if (prep.layered && st.layer_count > 1) {
        std::size_t const pauses = st.layer_count - 1;
        if (params.manual_pen_change) {
            duration_sec += static_cast<double>(pauses) * 12.0;
        } else if (params.enable_layer_tool_change_m6) {
            duration_sec += static_cast<double>(st.tool_change_count) * 8.0;
        } else if (params.auto_layer_pause_dwell_sec > 1e-9) {
            duration_sec += static_cast<double>(pauses) * params.auto_layer_pause_dwell_sec;
        }
    }
    st.estimated_duration_sec = duration_sec;
}

static SparseSamplingStrategy sparse_sampling_strategy_from_pref(Glib::ustring const &value)
{
    auto const lowered = Glib::ustring(value).lowercase();
    if (lowered == "legacy") {
        return SparseSamplingStrategy::Legacy;
    }
    return SparseSamplingStrategy::Directional;
}

static bool wants_layered_pause(GrblExportParams const &params, GrblExportContext const &ctx)
{
    if (!params.auto_pause_between_layers && !params.enable_layer_tool_change_m6) {
        return false;
    }
    if (!ctx.desktop) {
        return false;
    }
    if (ctx.selection && !ctx.selection->isEmpty()) {
        return false;
    }
    if (ctx.use_current_layer_without_selection) {
        return false;
    }
    return true;
}

static int parse_tool_id_from_layer_label(SPObject const *obj)
{
    if (!obj) {
        return -1;
    }
    char const *raw = obj->label();
    if (!raw || !*raw) {
        raw = obj->defaultLabel();
    }
    if (!raw || !*raw) {
        return -1;
    }
    static std::regex const tool_re(R"((?:^|[^A-Za-z0-9])T\s*([0-9]{1,3})(?:[^0-9]|$))", std::regex::icase);
    std::cmatch m;
    if (!std::regex_search(raw, m, tool_re)) {
        return -1;
    }
    try {
        return std::stoi(m[1].str());
    } catch (...) {
        return -1;
    }
}

static bool grbl_stage_debug_enabled()
{
    static bool const enabled = [] {
        if (auto const *value = std::getenv("INKSCAPE_GRBL_DEBUG_STAGES")) {
            return value[0] != '\0' && std::strcmp(value, "0") != 0;
        }
        return false;
    }();
    return enabled;
}

static char sanitize_debug_token_char(char ch)
{
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
        return ch;
    }
    return '_';
}

static std::string sanitize_debug_token(char const *value)
{
    std::string out = value ? value : "";
    std::transform(out.begin(), out.end(), out.begin(), sanitize_debug_token_char);
    return out;
}

static char const *grbl_stage_dump_dir()
{
    static char const *dir = []() -> char const * {
        if (auto const *value = std::getenv("INKSCAPE_GRBL_DEBUG_DUMP_DIR")) {
            return value[0] != '\0' ? value : nullptr;
        }
        return nullptr;
    }();
    return dir;
}

static double stroke_polyline_length(std::vector<Geom::Point> const &stroke)
{
    double length = 0.0;
    for (std::size_t i = 1; i < stroke.size(); ++i) {
        length += Geom::L2(stroke[i] - stroke[i - 1]);
    }
    return length;
}

static std::string build_grbl_stage_dump_path(char const *scope, char const *variant, char const *stage)
{
    auto const *dir = grbl_stage_dump_dir();
    if (!dir) {
        return {};
    }

    std::string path(dir);
    if (!path.empty()) {
        char const tail = path.back();
        if (tail != '\\' && tail != '/') {
            path.push_back('\\');
        }
    }

    path += "grbl-stage-";
    path += sanitize_debug_token(scope);
    path.push_back('-');
    path += sanitize_debug_token(variant);
    path.push_back('-');
    path += sanitize_debug_token(stage);
    path += ".tsv";
    return path;
}

static void dump_grbl_stage_strokes(char const *scope, char const *variant, char const *stage,
                                    std::vector<std::vector<Geom::Point>> const &strokes)
{
    auto const path = build_grbl_stage_dump_path(scope, variant, stage);
    if (path.empty()) {
        return;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }

    out.setf(std::ios::fixed);
    out << std::setprecision(6);
    out << "stroke_index\tpoint_count\tclosed\tstart_x_mm\tstart_y_mm\tend_x_mm\tend_y_mm\tdraw_mm\n";
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        auto const &stroke = strokes[i];
        if (stroke.size() < 2) {
            continue;
        }
        out << i
            << '\t' << stroke.size()
            << '\t' << (stroke_is_closed_for_reorder(stroke) ? 1 : 0)
            << '\t' << stroke.front()[Geom::X]
            << '\t' << stroke.front()[Geom::Y]
            << '\t' << stroke.back()[Geom::X]
            << '\t' << stroke.back()[Geom::Y]
            << '\t' << stroke_polyline_length(stroke)
            << '\n';
    }
}

static void dump_grbl_stage_strokes_layers(char const *scope, char const *variant, char const *stage,
                                           std::vector<std::vector<std::vector<Geom::Point>>> const &layers,
                                           std::vector<int> const &tool_ids)
{
    auto const path = build_grbl_stage_dump_path(scope, variant, stage);
    if (path.empty()) {
        return;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }

    out.setf(std::ios::fixed);
    out << std::setprecision(6);
    out << "layer_index\ttool_id\tstroke_index\tpoint_count\tclosed\tstart_x_mm\tstart_y_mm\tend_x_mm\tend_y_mm\tdraw_mm\n";
    for (std::size_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
        auto const &layer = layers[layer_index];
        int const tool_id = layer_index < tool_ids.size() ? tool_ids[layer_index] : -1;
        for (std::size_t stroke_index = 0; stroke_index < layer.size(); ++stroke_index) {
            auto const &stroke = layer[stroke_index];
            if (stroke.size() < 2) {
                continue;
            }
            out << layer_index
                << '\t' << tool_id
                << '\t' << stroke_index
                << '\t' << stroke.size()
                << '\t' << (stroke_is_closed_for_reorder(stroke) ? 1 : 0)
                << '\t' << stroke.front()[Geom::X]
                << '\t' << stroke.front()[Geom::Y]
                << '\t' << stroke.back()[Geom::X]
                << '\t' << stroke.back()[Geom::Y]
                << '\t' << stroke_polyline_length(stroke)
                << '\n';
        }
    }
}

static void log_grbl_stage_stats(char const *scope, char const *variant, char const *stage,
                                 std::vector<std::vector<Geom::Point>> const &strokes, GrblExportParams const &params)
{
    if (!grbl_stage_debug_enabled()) {
        return;
    }

    PreparedPlotMm prep;
    prep.layered = false;
    prep.flat_mm = strokes;

    GrblPlotStats st;
    fill_grbl_plot_stats_from_prep(prep, params, st);

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << std::setprecision(3);
    oss << "[grbl-stage] scope=" << scope
        << " variant=" << variant
        << " stage=" << stage
        << " strokes=" << st.stroke_count;
    if (st.has_length_stats) {
        oss << " draw-mm=" << st.draw_length_mm
            << " travel-mm=" << st.travel_length_mm;
    }
    if (st.has_bounds_mm) {
        oss << " bbox-mm=[" << st.min_x_mm << "," << st.min_y_mm
            << "]-[" << st.max_x_mm << "," << st.max_y_mm << "]";
    }
    std::cerr << oss.str() << std::endl;
    dump_grbl_stage_strokes(scope, variant, stage, strokes);
}

static void log_grbl_stage_stats_layers(char const *scope, char const *variant, char const *stage,
                                        std::vector<std::vector<std::vector<Geom::Point>>> const &layers,
                                        std::vector<int> const &tool_ids, GrblExportParams const &params)
{
    if (!grbl_stage_debug_enabled()) {
        return;
    }

    PreparedPlotMm prep;
    prep.layered = true;
    prep.layers_mm = layers;
    prep.layer_tool_ids = tool_ids;

    GrblPlotStats st;
    fill_grbl_plot_stats_from_prep(prep, params, st);

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << std::setprecision(3);
    oss << "[grbl-stage] scope=" << scope
        << " variant=" << variant
        << " stage=" << stage
        << " layers=" << st.layer_count
        << " strokes=" << st.stroke_count;
    if (st.has_length_stats) {
        oss << " draw-mm=" << st.draw_length_mm
            << " travel-mm=" << st.travel_length_mm;
    }
    if (st.has_bounds_mm) {
        oss << " bbox-mm=[" << st.min_x_mm << "," << st.min_y_mm
            << "]-[" << st.max_x_mm << "," << st.max_y_mm << "]";
    }
    std::cerr << oss.str() << std::endl;
    dump_grbl_stage_strokes_layers(scope, variant, stage, layers, tool_ids);
}

static void collect_layers_doc_strokes(SPDesktop *desktop, SPDocument *doc, double const flatness,
                                       std::vector<std::vector<std::vector<Geom::Point>>> &layers_doc,
                                       std::vector<int> *layer_tool_ids = nullptr,
                                       std::vector<Glib::ustring> *layer_labels = nullptr)
{
    layers_doc.clear();
    if (layer_tool_ids) {
        layer_tool_ids->clear();
    }
    if (layer_labels) {
        layer_labels->clear();
    }
    std::list<SPItem *> const layer_items = desktop->layerManager().getAllLayers();
    for (SPItem *it : layer_items) {
        if (!it) {
            continue;
        }
        if (auto *obj = cast<SPObject>(it)) {
            std::vector<std::vector<Geom::Point>> layer_strokes;
            collect_shapes_recursive(obj, flatness, layer_strokes);
            layer_strokes.erase(std::remove_if(layer_strokes.begin(), layer_strokes.end(),
                                              [](std::vector<Geom::Point> const &s) { return s.size() < 2; }),
                                layer_strokes.end());
            if (!layer_strokes.empty()) {
                layers_doc.push_back(std::move(layer_strokes));
                if (layer_tool_ids) {
                    layer_tool_ids->push_back(parse_tool_id_from_layer_label(obj));
                }
                if (layer_labels) {
                    char const *raw = obj->label();
                    if (!raw || !*raw) {
                        raw = obj->defaultLabel();
                    }
                    layer_labels->push_back(raw ? Glib::ustring(raw) : Glib::ustring());
                }
            }
        }
    }
    if (layers_doc.empty()) {
        std::vector<std::vector<Geom::Point>> root;
        collect_shapes_recursive(doc->getRoot(), flatness, root);
        root.erase(std::remove_if(root.begin(), root.end(),
                                   [](std::vector<Geom::Point> const &s) { return s.size() < 2; }),
                    root.end());
        if (!root.empty()) {
            layers_doc.push_back(std::move(root));
            if (layer_tool_ids) {
                layer_tool_ids->push_back(-1);
            }
            if (layer_labels) {
                layer_labels->push_back(_("Root"));
            }
        }
    }
}

static void apply_flip_y_to_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers, double page_h_mm)
{
    for (auto &layer : layers) {
        apply_flip_y_canvas(layer, page_h_mm);
    }
}

static void apply_axis_mapping_to_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers, bool swap_xy,
                                         bool invert_x, bool invert_y, double machine_bed_width_mm,
                                         double machine_bed_depth_mm)
{
    for (auto &layer : layers) {
        apply_axis_mapping(layer, swap_xy, invert_x, invert_y, machine_bed_width_mm, machine_bed_depth_mm);
    }
}

static bool apply_align_min_to_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers, double *shift_x_out,
                                      double *shift_y_out)
{
    double minx = std::numeric_limits<double>::infinity();
    double miny = std::numeric_limits<double>::infinity();
    for (auto const &layer : layers) {
        for (auto const &st : layer) {
            for (auto const &p : st) {
                minx = std::min(minx, p[Geom::X]);
                miny = std::min(miny, p[Geom::Y]);
            }
        }
    }
    if (!std::isfinite(minx) || !std::isfinite(miny)) {
        if (shift_x_out) {
            *shift_x_out = 0;
        }
        if (shift_y_out) {
            *shift_y_out = 0;
        }
        return false;
    }
    if (shift_x_out) {
        *shift_x_out = minx;
    }
    if (shift_y_out) {
        *shift_y_out = miny;
    }
    for (auto &layer : layers) {
        for (auto &st : layer) {
            for (auto &p : st) {
                p[Geom::X] -= minx;
                p[Geom::Y] -= miny;
            }
        }
    }
    return true;
}

static void prune_empty_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers,
                               std::vector<int> *layer_tool_ids = nullptr,
                               std::vector<Glib::ustring> *layer_labels = nullptr)
{
    std::vector<std::vector<std::vector<Geom::Point>>> kept_layers;
    std::vector<int> kept_tools;
    std::vector<Glib::ustring> kept_labels;
    kept_layers.reserve(layers.size());
    if (layer_tool_ids) {
        kept_tools.reserve(layer_tool_ids->size());
    }
    if (layer_labels) {
        kept_labels.reserve(layer_labels->size());
    }

    for (std::size_t i = 0; i < layers.size(); ++i) {
        bool const empty = std::none_of(layers[i].begin(), layers[i].end(),
                                        [](std::vector<Geom::Point> const &s) { return s.size() >= 2; });
        if (empty) {
            continue;
        }
        kept_layers.push_back(std::move(layers[i]));
        if (layer_tool_ids && i < layer_tool_ids->size()) {
            kept_tools.push_back((*layer_tool_ids)[i]);
        }
        if (layer_labels && i < layer_labels->size()) {
            kept_labels.push_back((*layer_labels)[i]);
        }
    }

    layers = std::move(kept_layers);
    if (layer_tool_ids) {
        *layer_tool_ids = std::move(kept_tools);
    }
    if (layer_labels) {
        *layer_labels = std::move(kept_labels);
    }
}

static void coalesce_consecutive_same_tool_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers,
                                                  std::vector<int> *layer_tool_ids = nullptr,
                                                  std::vector<Glib::ustring> *layer_labels = nullptr)
{
    if (layers.size() <= 1 || !layer_tool_ids || layer_tool_ids->size() != layers.size()) {
        return;
    }

    std::vector<std::vector<std::vector<Geom::Point>>> merged_layers;
    std::vector<int> merged_tools;
    std::vector<Glib::ustring> merged_labels;
    merged_layers.reserve(layers.size());
    merged_tools.reserve(layer_tool_ids->size());
    if (layer_labels) {
        merged_labels.reserve(layer_labels->size());
    }

    for (std::size_t i = 0; i < layers.size(); ++i) {
        int const tool_id = (*layer_tool_ids)[i];
        if (!merged_layers.empty() && tool_id >= 0 && merged_tools.back() == tool_id) {
            auto &dst = merged_layers.back();
            auto &src = layers[i];
            dst.insert(dst.end(), std::make_move_iterator(src.begin()), std::make_move_iterator(src.end()));
            if (layer_labels && i < layer_labels->size() && !(*layer_labels)[i].empty()) {
                if (merged_labels.back().empty()) {
                    merged_labels.back() = (*layer_labels)[i];
                } else {
                    merged_labels.back() += " + ";
                    merged_labels.back() += (*layer_labels)[i];
                }
            }
            continue;
        }

        merged_layers.push_back(std::move(layers[i]));
        merged_tools.push_back(tool_id);
        if (layer_labels) {
            merged_labels.push_back(i < layer_labels->size() ? (*layer_labels)[i] : Glib::ustring());
        }
    }

    layers = std::move(merged_layers);
    *layer_tool_ids = std::move(merged_tools);
    if (layer_labels) {
        *layer_labels = std::move(merged_labels);
    }
}

static bool validate_layer_tool_ids_for_m6(PreparedPlotMm const &prep, std::string &err_out)
{
    if (!prep.layered || prep.layers_mm.empty()) {
        return true;
    }

    std::vector<Glib::ustring> missing_layers;
    missing_layers.reserve(prep.layers_mm.size());
    for (std::size_t i = 0; i < prep.layers_mm.size(); ++i) {
        bool const has_strokes = std::any_of(prep.layers_mm[i].begin(), prep.layers_mm[i].end(),
                                             [](std::vector<Geom::Point> const &s) { return s.size() >= 2; });
        if (!has_strokes) {
            continue;
        }

        int const tool_id = i < prep.layer_tool_ids.size() ? prep.layer_tool_ids[i] : -1;
        if (tool_id >= 0) {
            continue;
        }

        if (i < prep.layer_labels.size() && !prep.layer_labels[i].empty()) {
            missing_layers.push_back(prep.layer_labels[i]);
        } else {
            missing_layers.push_back(Glib::ustring::compose(_("图层 #%1"), static_cast<guint64>(i + 1)));
        }
    }

    if (missing_layers.empty()) {
        return true;
    }

    std::ostringstream labels;
    std::size_t const preview_count = std::min<std::size_t>(missing_layers.size(), 6);
    for (std::size_t i = 0; i < preview_count; ++i) {
        if (i > 0) {
            labels << "，";
        }
        labels << missing_layers[i];
    }
    if (missing_layers.size() > preview_count) {
        labels << " ...";
    }

    err_out = Glib::ustring::compose(
                  _("当前选择了“按图层工具号换笔(M6)”，但以下非空图层缺少 Tn 工具号：%1。"
                    "为避免用错笔，已停止生成/发送。请给每个要绘制的图层补上 T1/T2/T3，或改用“手动换笔”模式。"),
                  Glib::ustring(labels.str()))
                  .raw();
    return false;
}

static std::string build_empty_after_mapping_message(GrblExportParams const &params)
{
    std::ostringstream msg;
    msg << _("按当前坐标映射与床面裁剪设置，图稿最终没有任何可绘制笔画。");
    msg << " ";
    msg << _("常见原因：开启了“限制在机器床面内”，但图稿经过交换轴、反转、页面 Y 镜像或原点对齐后，整体落在床面范围之外。");
    if (params.clip_to_machine_bed) {
        std::ostringstream dims;
        dims << std::fixed << std::setprecision(1) << params.machine_bed_width_mm << " x "
             << params.machine_bed_depth_mm;
        msg << " ";
        msg << Glib::ustring::compose(_("当前床面尺寸：%1 mm。"), Glib::ustring(dims.str())).raw();
    }
    msg << " ";
    msg << _("建议先尝试：关闭“限制在机器床面内”，或开启“左下角对齐到机器原点”，再检查交换 X/Y、反转 X/Y、页面 Y 镜像是否设置正确。");
    return msg.str();
}

static bool fill_prepared_plot_mm(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                                  PreparedPlotMm &prep, std::string &err_out)
{
    prep.layered = false;
    prep.flat_mm.clear();
    prep.layers_mm.clear();
    prep.layer_tool_ids.clear();
    prep.layer_labels.clear();
    prep.preview_flip_y_applied = false;
    prep.preview_page_h_mm = 0;
    prep.preview_align_applied = false;
    prep.preview_align_shift_x_mm = 0;
    prep.preview_align_shift_y_mm = 0;
    prep.preview_swap_xy_applied = false;
    prep.preview_invert_x_applied = false;
    prep.preview_invert_y_applied = false;
    prep.preview_clip_applied = false;
    prep.preview_machine_bed_width_mm = params.machine_bed_width_mm;
    prep.preview_machine_bed_depth_mm = params.machine_bed_depth_mm;

    if (!doc) {
        err_out = _("Invalid document.");
        return false;
    }
    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }

    auto const mapper = build_document_mm_mapper(doc);

    if (wants_layered_pause(params, ctx)) {
        std::vector<std::vector<std::vector<Geom::Point>>> layers_doc;
        collect_layers_doc_strokes(ctx.desktop, doc, params.flatness, layers_doc, &prep.layer_tool_ids, &prep.layer_labels);
        if (layers_doc.size() > 1) {
            if (!params.auto_pause_between_layers && params.enable_layer_tool_change_m6) {
                coalesce_consecutive_same_tool_layers(layers_doc, &prep.layer_tool_ids, &prep.layer_labels);
            }
            prep.layered = true;
            auto layers_doc_baseline = layers_doc;
            prep.layers_mm.resize(layers_doc.size());
            std::vector<std::vector<std::vector<Geom::Point>>> layers_mm_baseline(layers_doc.size());
            for (std::size_t i = 0; i < layers_doc.size(); ++i) {
                // Keep the source stroke order intact until after geometry-changing stages such as sparse sampling.
                // This keeps "optimize stroke order" from changing which strokes survive order-based sampling.
                strokes_doc_to_mm(layers_doc[i], mapper, prep.layers_mm[i]);
                strokes_doc_to_mm(layers_doc_baseline[i], mapper, layers_mm_baseline[i]);
            }
            if (params.contour_to_hatch) {
                convert_closed_contours_to_hatch_layers(prep.layers_mm, params.hatch_spacing_mm,
                                                        params.hatch_angle_deg, params.hatch_cross,
                                                        params.hatch_inset_enable, params.hatch_inset_mm,
                                                        params.hatch_angle_increment_enable,
                                                        params.hatch_angle_increment_deg, &prep.hatch_stats);
                convert_closed_contours_to_hatch_layers(layers_mm_baseline, params.hatch_spacing_mm,
                                                        params.hatch_angle_deg, params.hatch_cross,
                                                        params.hatch_inset_enable, params.hatch_inset_mm,
                                                        params.hatch_angle_increment_enable,
                                                        params.hatch_angle_increment_deg);
            }
            log_grbl_stage_stats_layers("layered", "optimized", "after-doc-to-mm", prep.layers_mm, prep.layer_tool_ids,
                                        params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-doc-to-mm", layers_mm_baseline, prep.layer_tool_ids, params);
            double const page_h = document_page_height_mm(doc);
            if (params.flip_y_canvas) {
                apply_flip_y_to_layers(prep.layers_mm, page_h);
                apply_flip_y_to_layers(layers_mm_baseline, page_h);
                prep.preview_flip_y_applied = true;
                prep.preview_page_h_mm = page_h;
            }
            apply_axis_mapping_to_layers(prep.layers_mm, params.swap_xy, params.invert_x, params.invert_y,
                                         params.machine_bed_width_mm, params.machine_bed_depth_mm);
            apply_axis_mapping_to_layers(layers_mm_baseline, params.swap_xy, params.invert_x, params.invert_y,
                                         params.machine_bed_width_mm, params.machine_bed_depth_mm);
            prep.preview_swap_xy_applied = params.swap_xy;
            prep.preview_invert_x_applied = params.invert_x;
            prep.preview_invert_y_applied = params.invert_y;
            log_grbl_stage_stats_layers("layered", "optimized", "after-mapping", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-mapping", layers_mm_baseline, prep.layer_tool_ids, params);
            if (params.align_content_min_to_origin) {
                double shx = 0;
                double shy = 0;
                if (apply_align_min_to_layers(prep.layers_mm, &shx, &shy)) {
                    prep.preview_align_applied = true;
                    prep.preview_align_shift_x_mm = shx;
                    prep.preview_align_shift_y_mm = shy;
                }
                apply_align_min_to_layers(layers_mm_baseline, nullptr, nullptr);
            }
            log_grbl_stage_stats_layers("layered", "optimized", "after-align", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-align", layers_mm_baseline, prep.layer_tool_ids, params);
            if (params.clip_to_machine_bed && params.machine_bed_width_mm > 1e-6 && params.machine_bed_depth_mm > 1e-6) {
                auto const [clip_bed_width_mm, clip_bed_depth_mm] =
                    get_mapped_machine_bed_dims(params.swap_xy, params.machine_bed_width_mm, params.machine_bed_depth_mm);
                for (auto &layer : prep.layers_mm) {
                    clip_strokes_to_axis_rect(layer, 0, 0, clip_bed_width_mm, clip_bed_depth_mm);
                }
                for (auto &layer : layers_mm_baseline) {
                    clip_strokes_to_axis_rect(layer, 0, 0, clip_bed_width_mm, clip_bed_depth_mm);
                }
                prep.preview_clip_applied = true;
            }
            log_grbl_stage_stats_layers("layered", "optimized", "after-clip", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-clip", layers_mm_baseline, prep.layer_tool_ids, params);
            if (params.enable_sparse_stroke_sampling) {
                sparse_sample_strokes_layers(prep.layers_mm, params.sparse_keep_every, params.sparse_sampling_strategy);
                sparse_sample_strokes_layers(layers_mm_baseline, params.sparse_keep_every,
                                             params.sparse_sampling_strategy);
            }
            log_grbl_stage_stats_layers("layered", "optimized", "after-sparse", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-sparse", layers_mm_baseline, prep.layer_tool_ids, params);
            if (params.optimize_stroke_order) {
                reorder_strokes_layers_quantized_for_grbl(prep.layers_mm, params.optimize_stroke_direction);
            }
            log_grbl_stage_stats_layers("layered", "optimized", "after-reorder", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-reorder", layers_mm_baseline, prep.layer_tool_ids, params);
            merge_exact_touching_strokes_layers(prep.layers_mm);
            merge_exact_touching_strokes_layers(layers_mm_baseline);
            log_grbl_stage_stats_layers("layered", "optimized", "after-exact-join", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-exact-join", layers_mm_baseline, prep.layer_tool_ids, params);
            if (params.optimize_stroke_order) {
                reorder_strokes_layers_nearest_neighbor(prep.layers_mm, params.optimize_stroke_direction);
            }
            log_grbl_stage_stats_layers("layered", "optimized", "after-exact-reorder", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-exact-reorder", layers_mm_baseline, prep.layer_tool_ids, params);
            if (params.enable_near_connect) {
                // Run bridge-joining after whole-stroke reordering so adjacent strokes are already spatial neighbors.
                connect_nearby_strokes_layers(prep.layers_mm, params.near_connect_distance_mm);
                connect_nearby_strokes_layers(layers_mm_baseline, params.near_connect_distance_mm);
                log_grbl_stage_stats_layers("layered", "optimized", "after-near-connect", prep.layers_mm, prep.layer_tool_ids, params);
                log_grbl_stage_stats_layers("layered", "baseline", "after-near-connect", layers_mm_baseline, prep.layer_tool_ids, params);
                if (params.optimize_stroke_order) {
                    // Near-connect can merge and reshape strokes, so give the merged result one more ordering pass.
                    reorder_strokes_layers_nearest_neighbor(prep.layers_mm, params.optimize_stroke_direction);
                }
            }
            log_grbl_stage_stats_layers("layered", "optimized", "after-near-reorder", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-near-reorder", layers_mm_baseline, prep.layer_tool_ids, params);
            if (params.enable_path_lead_in || params.enable_path_lead_out) {
                apply_open_stroke_lead_in_out_layers(prep.layers_mm, params.lead_in_distance_mm,
                                                     params.lead_out_distance_mm);
                apply_open_stroke_lead_in_out_layers(layers_mm_baseline, params.lead_in_distance_mm,
                                                     params.lead_out_distance_mm);
            }
            log_grbl_stage_stats_layers("layered", "optimized", "after-lead", prep.layers_mm, prep.layer_tool_ids, params);
            log_grbl_stage_stats_layers("layered", "baseline", "after-lead", layers_mm_baseline, prep.layer_tool_ids, params);
            if (params.optimize_stroke_order) {
                PreparedPlotMm baseline_prep;
                baseline_prep.layered = true;
                baseline_prep.layers_mm = layers_mm_baseline;
                baseline_prep.layer_tool_ids = prep.layer_tool_ids;
                double baseline_draw = 0.0;
                double baseline_travel = 0.0;
                if (compute_grbl_plot_lengths_from_prep(baseline_prep, params, baseline_draw, baseline_travel)) {
                    prep.has_travel_optimization_stats = true;
                    prep.travel_length_before_optimization_mm = baseline_travel;
                }
            }
            prune_empty_layers(prep.layers_mm, &prep.layer_tool_ids, &prep.layer_labels);
            if (prep.layers_mm.empty()) {
                err_out = build_empty_after_mapping_message(params);
                return false;
            }
            if (params.enable_layer_tool_change_m6 && !validate_layer_tool_ids_for_m6(prep, err_out)) {
                return false;
            }
            if (prep.layers_mm.size() <= 1) {
                prep.layered = false;
                prep.flat_mm = std::move(prep.layers_mm.front());
                prep.layers_mm.clear();
                prep.layer_tool_ids.clear();
                prep.layer_labels.clear();
            }
            return true;
        }
    }

    std::vector<std::vector<Geom::Point>> strokes_doc;
    collect_strokes_for_context(ctx, doc, params.flatness, strokes_doc);

    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }

    auto const strokes_doc_baseline = strokes_doc;
    constexpr std::size_t max_strokes = 200000;
    if (strokes_doc.size() > max_strokes) {
        strokes_doc.resize(max_strokes);
    }

    if (strokes_doc.empty()) {
        err_out = _("No drawable vector paths found (convert objects to paths if needed).");
        return false;
    }

    strokes_doc_to_mm(strokes_doc, mapper, prep.flat_mm);
    std::vector<std::vector<Geom::Point>> flat_mm_baseline;
    strokes_doc_to_mm(strokes_doc_baseline, mapper, flat_mm_baseline);
    if (params.contour_to_hatch) {
        convert_closed_contours_to_hatch(prep.flat_mm, params.hatch_spacing_mm, params.hatch_angle_deg,
                                         params.hatch_cross, params.hatch_inset_enable,
                                         params.hatch_inset_mm, params.hatch_angle_increment_enable,
                                         params.hatch_angle_increment_deg, &prep.hatch_stats);
        convert_closed_contours_to_hatch(flat_mm_baseline, params.hatch_spacing_mm, params.hatch_angle_deg,
                                         params.hatch_cross, params.hatch_inset_enable,
                                         params.hatch_inset_mm, params.hatch_angle_increment_enable,
                                         params.hatch_angle_increment_deg);
    }
    log_grbl_stage_stats("flat", "optimized", "after-doc-to-mm", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-doc-to-mm", flat_mm_baseline, params);
    if (prep.flat_mm.empty()) {
        err_out = _("No drawable vector paths found (convert objects to paths if needed).");
        return false;
    }

    if (params.flip_y_canvas) {
        double const ph = document_page_height_mm(doc);
        apply_flip_y_canvas(prep.flat_mm, ph);
        apply_flip_y_canvas(flat_mm_baseline, ph);
        prep.preview_flip_y_applied = true;
        prep.preview_page_h_mm = ph;
    }
    apply_axis_mapping(prep.flat_mm, params.swap_xy, params.invert_x, params.invert_y,
                       params.machine_bed_width_mm, params.machine_bed_depth_mm);
    apply_axis_mapping(flat_mm_baseline, params.swap_xy, params.invert_x, params.invert_y,
                       params.machine_bed_width_mm, params.machine_bed_depth_mm);
    prep.preview_swap_xy_applied = params.swap_xy;
    prep.preview_invert_x_applied = params.invert_x;
    prep.preview_invert_y_applied = params.invert_y;
    log_grbl_stage_stats("flat", "optimized", "after-mapping", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-mapping", flat_mm_baseline, params);
    if (params.align_content_min_to_origin) {
        double shx = 0;
        double shy = 0;
        if (apply_align_min_to_origin(prep.flat_mm, &shx, &shy)) {
            prep.preview_align_applied = true;
            prep.preview_align_shift_x_mm = shx;
            prep.preview_align_shift_y_mm = shy;
        }
        apply_align_min_to_origin(flat_mm_baseline, nullptr, nullptr);
    }
    log_grbl_stage_stats("flat", "optimized", "after-align", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-align", flat_mm_baseline, params);
    if (params.clip_to_machine_bed && params.machine_bed_width_mm > 1e-6 && params.machine_bed_depth_mm > 1e-6) {
        auto const [clip_bed_width_mm, clip_bed_depth_mm] =
            get_mapped_machine_bed_dims(params.swap_xy, params.machine_bed_width_mm, params.machine_bed_depth_mm);
        clip_strokes_to_axis_rect(prep.flat_mm, 0, 0, clip_bed_width_mm, clip_bed_depth_mm);
        clip_strokes_to_axis_rect(flat_mm_baseline, 0, 0, clip_bed_width_mm, clip_bed_depth_mm);
        prep.preview_clip_applied = true;
    }
    log_grbl_stage_stats("flat", "optimized", "after-clip", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-clip", flat_mm_baseline, params);
    if (params.enable_sparse_stroke_sampling) {
        sparse_sample_strokes(prep.flat_mm, params.sparse_keep_every, params.sparse_sampling_strategy);
        sparse_sample_strokes(flat_mm_baseline, params.sparse_keep_every, params.sparse_sampling_strategy);
    }
    log_grbl_stage_stats("flat", "optimized", "after-sparse", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-sparse", flat_mm_baseline, params);
    if (params.optimize_stroke_order && prep.flat_mm.size() > 1) {
        reorder_strokes_quantized_for_grbl(prep.flat_mm, params.optimize_stroke_direction);
    }
    log_grbl_stage_stats("flat", "optimized", "after-reorder", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-reorder", flat_mm_baseline, params);
    merge_exact_touching_strokes(prep.flat_mm);
    merge_exact_touching_strokes(flat_mm_baseline);
    log_grbl_stage_stats("flat", "optimized", "after-exact-join", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-exact-join", flat_mm_baseline, params);
    if (params.optimize_stroke_order && prep.flat_mm.size() > 1) {
        reorder_strokes_nearest_neighbor(prep.flat_mm, params.optimize_stroke_direction);
    }
    log_grbl_stage_stats("flat", "optimized", "after-exact-reorder", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-exact-reorder", flat_mm_baseline, params);
    if (params.enable_near_connect) {
        // Run bridge-joining after whole-stroke reordering so adjacent strokes are already spatial neighbors.
        connect_nearby_strokes(prep.flat_mm, params.near_connect_distance_mm);
        connect_nearby_strokes(flat_mm_baseline, params.near_connect_distance_mm);
        log_grbl_stage_stats("flat", "optimized", "after-near-connect", prep.flat_mm, params);
        log_grbl_stage_stats("flat", "baseline", "after-near-connect", flat_mm_baseline, params);
        if (params.optimize_stroke_order && prep.flat_mm.size() > 1) {
            // Near-connect can merge and reshape strokes, so give the merged result one more ordering pass.
            reorder_strokes_nearest_neighbor(prep.flat_mm, params.optimize_stroke_direction);
        }
    }
    log_grbl_stage_stats("flat", "optimized", "after-near-reorder", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-near-reorder", flat_mm_baseline, params);
    if (params.enable_path_lead_in || params.enable_path_lead_out) {
        apply_open_stroke_lead_in_out(prep.flat_mm, params.lead_in_distance_mm, params.lead_out_distance_mm);
        apply_open_stroke_lead_in_out(flat_mm_baseline, params.lead_in_distance_mm, params.lead_out_distance_mm);
    }
    log_grbl_stage_stats("flat", "optimized", "after-lead", prep.flat_mm, params);
    log_grbl_stage_stats("flat", "baseline", "after-lead", flat_mm_baseline, params);
    if (params.optimize_stroke_order) {
        PreparedPlotMm baseline_prep;
        baseline_prep.layered = false;
        baseline_prep.flat_mm = flat_mm_baseline;
        double baseline_draw = 0.0;
        double baseline_travel = 0.0;
        if (compute_grbl_plot_lengths_from_prep(baseline_prep, params, baseline_draw, baseline_travel)) {
            prep.has_travel_optimization_stats = true;
            prep.travel_length_before_optimization_mm = baseline_travel;
        }
    }

    if (prep.flat_mm.empty()) {
        err_out = build_empty_after_mapping_message(params);
        return false;
    }

    prep.layered = false;
    return true;
}

static bool fill_strokes_mm_for_export(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                                       std::vector<std::vector<Geom::Point>> &strokes_mm, std::string &err_out)
{
    PreparedPlotMm prep;
    if (!fill_prepared_plot_mm(doc, params, ctx, prep, err_out)) {
        strokes_mm.clear();
        return false;
    }
    if (prep.layered) {
        err_out = _("Internal error: layered plot requested but flat stroke buffer was used.");
        strokes_mm.clear();
        return false;
    }
    strokes_mm = std::move(prep.flat_mm);
    return true;
}

static std::size_t count_drawable_strokes_mm(std::vector<std::vector<Geom::Point>> const &strokes_mm)
{
    std::size_t n = 0;
    for (auto const &stroke : strokes_mm) {
        if (stroke.size() >= 2) {
            ++n;
        }
    }
    return n;
}

static std::size_t count_drawable_strokes_layers_mm(
    std::vector<std::vector<std::vector<Geom::Point>>> const &layers_mm)
{
    std::size_t n = 0;
    for (auto const &layer : layers_mm) {
        n += count_drawable_strokes_mm(layer);
    }
    return n;
}

/** Lines matching emit_strokes_flat (G21/G90, per-stroke pen/G0/pen/G1*, final pen up). */
static std::size_t estimate_emit_lines_flat(std::vector<std::vector<Geom::Point>> const &strokes_mm,
                                              [[maybe_unused]] GrblExportParams const &params)
{
    std::size_t n = 3 + count_custom_gcode_lines(params.start_gcode) + count_custom_gcode_lines(params.end_gcode);
    for (auto const &stroke : strokes_mm) {
        if (stroke.size() < 2) {
            continue;
        }
        n += 3 + (stroke.size() - 1);
    }
    n += 1;
    return n;
}

/** Lines matching emit_strokes_layered (layer pauses as in emit, then same per-stroke pattern). */
static std::size_t estimate_emit_lines_layered(
    PreparedPlotMm const &prep, GrblExportParams const &params)
{
    std::size_t n = 3 + count_custom_gcode_lines(params.start_gcode) + count_custom_gcode_lines(params.end_gcode);
    int active_tool = -1;
    for (std::size_t li = 0; li < prep.layers_mm.size(); ++li) {
        int const next_tool = li < prep.layer_tool_ids.size() ? prep.layer_tool_ids[li] : -1;
        bool const needs_tool_change =
            li > 0 && params.enable_layer_tool_change_m6 && next_tool >= 0 && next_tool != active_tool;
        if (li > 0 && (params.auto_pause_between_layers || needs_tool_change)) {
            n += 1; // pen up before pause handling
            if (needs_tool_change) {
                if (params.tool_change_use_point) {
                    n += 1;
                }
                n += 1; // Tn M6
                n += 1; // resume G0
            } else if (params.manual_pen_change) {
                if (params.pen_change_to_home) {
                    n += 1; // G0 to XY origin
                }
                n += 1; // G0 back to resume (after optional dialog — no G-code for the dialog)
            } else if (params.auto_layer_pause_dwell_sec > 1e-9) {
                n += 1; // G4 dwell
            }
        }
        for (auto const &stroke : prep.layers_mm[li]) {
            if (stroke.size() < 2) {
                continue;
            }
            n += 3 + (stroke.size() - 1);
        }
        if (next_tool >= 0) {
            active_tool = next_tool;
        }
    }
    n += 1;
    return n;
}

static bool emit_line_impl(SerialPort *port, std::string *gcode_out, std::size_t max_out_bytes,
                           GrblExportContext const &ctx, std::string &err_out, std::string const &line)
{
    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }
    if (line.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
        return for_each_trimmed_gcode_line(line, [&](std::string const &subline) {
            return emit_line_impl(port, gcode_out, max_out_bytes, ctx, err_out, subline);
        });
    }
    if (port) {
        if (!grbl_send_line(*port, line, err_out)) {
            return false;
        }
        ++ctx.plot_gcode_lines_sent;
        if (ctx.on_plot_gcode_line_progress && ctx.plot_gcode_lines_estimate > 0) {
            ctx.on_plot_gcode_line_progress(ctx.plot_gcode_lines_sent, ctx.plot_gcode_lines_estimate);
        }
        return true;
    }
    if (gcode_out->size() + line.size() + 1 > max_out_bytes) {
        err_out = _(
            "Generated G-code exceeds the preview size limit. Use “Send document to GRBL plotter…” from the "
            "main menu to stream without storing the full program here, or simplify the drawing.");
        return false;
    }
    *gcode_out += line;
    *gcode_out += '\n';
    return true;
}

static bool emit_custom_gcode_block(SerialPort *port, std::string *gcode_out, std::size_t max_out_bytes,
                                    GrblExportContext const &ctx, std::string &err_out, Glib::ustring const &block)
{
    return for_each_trimmed_gcode_line(block.raw(), [&](std::string const &line) {
        if (!emit_line_impl(port, gcode_out, max_out_bytes, ctx, err_out, line)) {
            return false;
        }
        return true;
    });
}

static bool emit_pen_gcode_block(SerialPort *port, std::string *gcode_out, std::size_t max_out_bytes,
                                 GrblExportContext const &ctx, std::string &err_out, Glib::ustring const &block)
{
    return emit_custom_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, block);
}

static bool emit_strokes_flat(SerialPort *port, std::string *gcode_out, std::size_t max_out_bytes,
                              std::vector<std::vector<Geom::Point>> const &strokes_mm, GrblExportParams const &params,
                              GrblExportContext const &ctx, std::string &err_out)
{
    if (!port && !gcode_out) {
        err_out = _("Internal error: no G-code output target.");
        return false;
    }

    auto emit_line = [&](std::string const &line) -> bool {
        return emit_line_impl(port, gcode_out, max_out_bytes, ctx, err_out, line);
    };

    if (!emit_line("G21")) {
        return false;
    }
    if (!emit_line("G90")) {
        return false;
    }
    // Keep work XY origin pinned to the current pen location, but do not remap Z.
    // Remapping Z with G92 makes pen-up / pen-down commands depend on the machine's
    // incidental starting height, which can leave a later `G1 Z5` effectively
    // stationary even though the controller returns `ok`.
    if (!emit_line("G92 X0 Y0")) {
        return false;
    }
    if (!emit_custom_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, params.start_gcode)) {
        return false;
    }

    Glib::ustring const pen_up = params.pen_up_cmd;
    Glib::ustring const pen_dn = params.pen_down_cmd;
    Glib::ustring long_pen_up = pen_up;
    if (params.enable_long_pen_up) {
        std::ostringstream z;
        z << std::fixed << std::setprecision(3) << params.long_pen_up_mm;
        long_pen_up = "G1 Z" + z.str() + " F3000";
    }

    std::size_t const stroke_total = count_drawable_strokes_mm(strokes_mm);
    std::size_t stroke_done = 0;
    Geom::Point prev_end(0, 0);
    bool has_prev_end = false;

    for (auto const &stroke : strokes_mm) {
        if (stroke.size() < 2) {
            continue;
        }

        double const travel_dist = Geom::L2(stroke.front() - (has_prev_end ? prev_end : Geom::Point(0, 0)));
        bool const use_long_pen_up = params.enable_long_pen_up && travel_dist >= params.long_move_distance_mm;
        if (!emit_pen_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, use_long_pen_up ? long_pen_up : pen_up)) {
            return false;
        }
        if (!emit_optional_dwell_ms(emit_line, params.pen_up_delay_ms)) {
            return false;
        }

        if (!has_prev_end || !Geom::are_near(stroke.front(), prev_end, k_machine_coord_epsilon_mm)) {
            if (!emit_line(format_xy_mm(stroke.front(), "G0", params.feed_travel_mm_min))) {
                return false;
            }
        }

        if (!emit_pen_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, pen_dn)) {
            return false;
        }
        if (!emit_optional_dwell_ms(emit_line, params.pen_down_delay_ms)) {
            return false;
        }

        for (size_t i = 1; i < stroke.size(); ++i) {
            if (!emit_line(format_xy_mm(stroke[i], "G1", params.feed_draw_mm_min))) {
                return false;
            }
        }
        ++stroke_done;
        prev_end = stroke.back();
        has_prev_end = true;
        if (ctx.on_plot_stroke_progress && stroke_total > 0) {
            ctx.on_plot_stroke_progress(stroke_done, stroke_total);
        }
    }

    if (!emit_pen_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, pen_up)) {
        return false;
    }
    if (!emit_optional_dwell_ms(emit_line, params.pen_up_delay_ms)) {
        return false;
    }
    if (!emit_custom_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, params.end_gcode)) {
        return false;
    }

    return true;
}

static bool emit_strokes_layered(SerialPort *port, std::string *gcode_out, std::size_t max_out_bytes,
                                 PreparedPlotMm const &prep,
                                 GrblExportParams const &params, GrblExportContext const &ctx, std::string &err_out)
{
    if (!port && !gcode_out) {
        err_out = _("Internal error: no G-code output target.");
        return false;
    }

    auto emit_line = [&](std::string const &line) -> bool {
        return emit_line_impl(port, gcode_out, max_out_bytes, ctx, err_out, line);
    };

    if (!emit_line("G21")) {
        return false;
    }
    if (!emit_line("G90")) {
        return false;
    }
    // Keep work XY origin pinned to the current pen location, but do not remap Z.
    // Remapping Z with G92 makes pen-up / pen-down commands depend on the machine's
    // incidental starting height, which can leave a later `G1 Z5` effectively
    // stationary even though the controller returns `ok`.
    if (!emit_line("G92 X0 Y0")) {
        return false;
    }
    if (!emit_custom_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, params.start_gcode)) {
        return false;
    }

    Glib::ustring const pen_up = params.pen_up_cmd;
    Glib::ustring const pen_dn = params.pen_down_cmd;
    Glib::ustring long_pen_up = pen_up;
    if (params.enable_long_pen_up) {
        std::ostringstream z;
        z << std::fixed << std::setprecision(3) << params.long_pen_up_mm;
        long_pen_up = "G1 Z" + z.str() + " F3000";
    }

    Geom::Point last_mm(0, 0);
    bool has_last = false;
    int active_tool = -1;

    std::size_t const stroke_total = count_drawable_strokes_layers_mm(prep.layers_mm);
    std::size_t stroke_done = 0;

    auto emit_one_layer = [&](std::vector<std::vector<Geom::Point>> const &strokes_mm) -> bool {
        for (auto const &stroke : strokes_mm) {
            if (stroke.size() < 2) {
                continue;
            }
            double const travel_dist = Geom::L2(stroke.front() - (has_last ? last_mm : Geom::Point(0, 0)));
            bool const use_long_pen_up = params.enable_long_pen_up && travel_dist >= params.long_move_distance_mm;
            if (!emit_pen_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, use_long_pen_up ? long_pen_up : pen_up)) {
                return false;
            }
            if (!emit_optional_dwell_ms(emit_line, params.pen_up_delay_ms)) {
                return false;
            }
            if (!has_last || !Geom::are_near(stroke.front(), last_mm, k_machine_coord_epsilon_mm)) {
                if (!emit_line(format_xy_mm(stroke.front(), "G0", params.feed_travel_mm_min))) {
                    return false;
                }
            }
            if (!emit_pen_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, pen_dn)) {
                return false;
            }
            if (!emit_optional_dwell_ms(emit_line, params.pen_down_delay_ms)) {
                return false;
            }
            for (size_t i = 1; i < stroke.size(); ++i) {
                if (!emit_line(format_xy_mm(stroke[i], "G1", params.feed_draw_mm_min))) {
                    return false;
                }
            }
            last_mm = stroke.back();
            has_last = true;
            ++stroke_done;
            if (ctx.on_plot_stroke_progress && stroke_total > 0) {
                ctx.on_plot_stroke_progress(stroke_done, stroke_total);
            }
        }
        return true;
    };

    for (std::size_t li = 0; li < prep.layers_mm.size(); ++li) {
        int const next_tool = li < prep.layer_tool_ids.size() ? prep.layer_tool_ids[li] : -1;
        bool const needs_tool_change =
            li > 0 && params.enable_layer_tool_change_m6 && next_tool >= 0 && next_tool != active_tool;
        if (li > 0 && (params.auto_pause_between_layers || needs_tool_change)) {
            if (!has_last) {
                err_out = _("Internal error: missing resume position for layer pause.");
                return false;
            }
            Geom::Point const resume = last_mm;

            if (!emit_pen_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, pen_up)) {
                return false;
            }
            if (!emit_optional_dwell_ms(emit_line, params.pen_up_delay_ms)) {
                return false;
            }

            if (needs_tool_change) {
                if (params.tool_change_use_point) {
                    Geom::Point const change(params.tool_change_x_mm, params.tool_change_y_mm);
                    if (!emit_line(format_xy_mm(change, "G0", params.feed_travel_mm_min))) {
                        return false;
                    }
                }
                {
                    std::ostringstream tline;
                    tline << "T" << next_tool << " M6";
                    if (!emit_line(tline.str())) {
                        return false;
                    }
                }
                if (!emit_line(format_xy_mm(resume, "G0", params.feed_travel_mm_min))) {
                    return false;
                }
            } else if (params.manual_pen_change) {
                if (params.pen_change_to_home) {
                    Geom::Point const origin(0, 0);
                    if (!emit_line(format_xy_mm(origin, "G0", params.feed_travel_mm_min))) {
                        return false;
                    }
                }
                if (ctx.inhibit_interactive_pen_changes && gcode_out) {
                    if (!emit_line("; (pause: manual pen change — interactive step omitted in text preview)")) {
                        return false;
                    }
                } else if (params.pen_change_prompt && ctx.on_manual_pen_change_between_layers) {
                    if (!ctx.on_manual_pen_change_between_layers(resume[Geom::X], resume[Geom::Y])) {
                        err_out = _("Pen change was cancelled.");
                        return false;
                    }
                }
                if (!emit_line(format_xy_mm(resume, "G0", params.feed_travel_mm_min))) {
                    return false;
                }
            } else if (params.auto_layer_pause_dwell_sec > 1e-9) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "G4 P%.3f", params.auto_layer_pause_dwell_sec);
                if (!emit_line(buf)) {
                    return false;
                }
            }
        }

        if (!emit_one_layer(prep.layers_mm[li])) {
            return false;
        }
        if (next_tool >= 0) {
            active_tool = next_tool;
        }
    }

    if (!emit_pen_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, pen_up)) {
        return false;
    }
    if (!emit_optional_dwell_ms(emit_line, params.pen_up_delay_ms)) {
        return false;
    }
    if (!emit_custom_gcode_block(port, gcode_out, max_out_bytes, ctx, err_out, params.end_gcode)) {
        return false;
    }

    return true;
}

static bool emit_grbl_program_dispatch(SerialPort *port, std::string *gcode_out, std::size_t max_out_bytes,
                                       PreparedPlotMm const &prep, GrblExportParams const &params,
                                       GrblExportContext const &ctx, std::string &err_out)
{
    ctx.plot_gcode_lines_sent = 0;
    if (!prep.layered) {
        ctx.plot_gcode_lines_estimate = estimate_emit_lines_flat(prep.flat_mm, params);
        return emit_strokes_flat(port, gcode_out, max_out_bytes, prep.flat_mm, params, ctx, err_out);
    }
    ctx.plot_gcode_lines_estimate = estimate_emit_lines_layered(prep, params);
    return emit_strokes_layered(port, gcode_out, max_out_bytes, prep, params, ctx, err_out);
}

bool export_paths_to_grbl(SerialPort &port, SPDocument *doc, GrblExportParams const &params,
                          GrblExportContext const &ctx, std::string &err_out, std::size_t *stroke_count_out,
                          GrblPlotStats *stats_out)
{
    err_out.clear();
    if (stroke_count_out) {
        *stroke_count_out = 0;
    }
    if (stats_out) {
        *stats_out = {};
    }
    if (!port.is_open() || !doc) {
        err_out = _("Invalid serial port or document.");
        return false;
    }
    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }

    PreparedPlotMm prep;
    if (!fill_prepared_plot_mm(doc, params, ctx, prep, err_out)) {
        return false;
    }

    if (stroke_count_out) {
        *stroke_count_out = prep.count_strokes();
    }
    if (stats_out) {
        fill_grbl_plot_stats_from_prep(prep, params, *stats_out);
    }

    return emit_grbl_program_dispatch(&port, nullptr, std::numeric_limits<std::size_t>::max(), prep, params, ctx,
                                       err_out);
}

bool analyze_grbl_plot(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                       GrblPlotStats &stats_out, std::string &err_out)
{
    err_out.clear();
    stats_out = {};
    if (!doc) {
        err_out = _("Invalid document.");
        return false;
    }
    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }

    PreparedPlotMm prep;
    if (!fill_prepared_plot_mm(doc, params, ctx, prep, err_out)) {
        return false;
    }

    fill_grbl_plot_stats_from_prep(prep, params, stats_out);
    return true;
}

void grbl_export_params_from_preferences(Inkscape::Preferences *prefs, GrblExportParams &params)
{
    if (!prefs) {
        return;
    }
    migrate_clip_to_machine_bed_default(prefs);
    migrate_end_gcode_default(prefs);
    migrate_pen_z_absolute_default(prefs);
    constexpr auto k_flat = "/options/grbl/flatness";
    constexpr auto k_fd = "/options/grbl/feed-draw-mmmin";
    constexpr auto k_ft = "/options/grbl/feed-travel-mmmin";
    constexpr auto k_pen_up = "/options/grbl/pen-up-cmd";
    constexpr auto k_pen_dn = "/options/grbl/pen-down-cmd";
    constexpr auto k_pen_up_delay = "/options/grbl/pen-up-delay-ms";
    constexpr auto k_pen_down_delay = "/options/grbl/pen-down-delay-ms";
    constexpr auto k_lead_in = "/options/grbl/enable-path-lead-in";
    constexpr auto k_lead_in_dist = "/options/grbl/path-lead-in-distance-mm";
    constexpr auto k_lead_out = "/options/grbl/enable-path-lead-out";
    constexpr auto k_lead_out_dist = "/options/grbl/path-lead-out-distance-mm";
    constexpr auto k_lead_in_out = "/options/grbl/enable-path-lead-in-out";
    constexpr auto k_lead_in_out_dist = "/options/grbl/path-lead-in-out-distance-mm";
    constexpr auto k_pen_ctl = "/options/grbl/pen-control";
    constexpr auto k_long_pen_up = "/options/grbl/enable-long-pen-up";
    constexpr auto k_long_pen_up_mm = "/options/grbl/long-pen-up-mm";
    constexpr auto k_long_move_dist = "/options/grbl/long-move-dist-mm";
    constexpr auto k_near_connect = "/options/grbl/enable-near-connect";
    constexpr auto k_near_connect_dist = "/options/grbl/near-connect-distance-mm";
    constexpr auto k_sparse_sampling = "/options/grbl/enable-sparse-stroke-sampling";
    constexpr auto k_sparse_keep_every = "/options/grbl/sparse-keep-every";
    constexpr auto k_sparse_strategy = "/options/grbl/sparse-strategy";
    constexpr auto k_opt = "/options/grbl/optimize-stroke-order";
    constexpr auto k_opt_dir = "/options/grbl/optimize-stroke-direction";
    constexpr auto k_hatch = "/options/grbl/contour-to-hatch";
    constexpr auto k_hatch_spacing = "/options/grbl/hatch-spacing-mm";
    constexpr auto k_hatch_angle = "/options/grbl/hatch-angle-deg";
    constexpr auto k_hatch_cross = "/options/grbl/hatch-cross";
    constexpr auto k_hatch_inset_enable = "/options/grbl/hatch-inset-enable";
    constexpr auto k_hatch_inset_mm = "/options/grbl/hatch-inset-mm";
    constexpr auto k_hatch_angle_increment_enable = "/options/grbl/hatch-angle-increment-enable";
    constexpr auto k_hatch_angle_increment_deg = "/options/grbl/hatch-angle-increment-deg";
    constexpr auto k_flip = "/options/grbl/flip-y-canvas";
    constexpr auto k_swap_xy = "/options/grbl/swap-xy";
    constexpr auto k_invert_x = "/options/grbl/invert-x";
    constexpr auto k_invert_y = "/options/grbl/invert-y";
    constexpr auto k_align = "/options/grbl/align-content-min";
    constexpr auto k_bw = "/options/grbl/machine-bed-width-mm";
    constexpr auto k_bd = "/options/grbl/machine-bed-depth-mm";
    constexpr auto k_autopause = "/options/grbl/auto-pause-between-layers";
    constexpr auto k_manual_pen = "/options/grbl/manual-pen-change";
    constexpr auto k_pen_ch_home = "/options/grbl/pen-change-to-home";
    constexpr auto k_pen_ch_prompt = "/options/grbl/pen-change-prompt";
    constexpr auto k_tool_change_m6 = "/options/grbl/enable-layer-tool-change-m6";
    constexpr auto k_tool_change_point = "/options/grbl/tool-change-use-point";
    constexpr auto k_tool_change_x = "/options/grbl/tool-change-x-mm";
    constexpr auto k_tool_change_y = "/options/grbl/tool-change-y-mm";
    constexpr auto k_layer_dwell = "/options/grbl/auto-layer-pause-dwell-sec";
    constexpr auto k_start_gcode = "/options/grbl/start-gcode";

    params.flatness = prefs->getDoubleLimited(k_flat, 0.08, 0.001, 10.0);
    params.feed_draw_mm_min = prefs->getDoubleLimited(k_fd, 1200.0, 60.0, 12000.0);
    params.feed_travel_mm_min = prefs->getDoubleLimited(k_ft, 6000.0, 60.0, 20000.0);
    {
        Glib::ustring const pctl = prefs->getString(k_pen_ctl, "z");
        if (pctl == "m3m5" || pctl == "M3M5") {
            params.pen_up_cmd = "M5";
            params.pen_down_cmd = "M3 S1000";
            params.enable_long_pen_up = false;
        } else {
            params.pen_up_cmd = prefs->getString(k_pen_up, k_default_pen_up_gcode);
            params.pen_down_cmd = prefs->getString(k_pen_dn, k_default_pen_down_gcode);
            params.enable_long_pen_up = prefs->getBool(k_long_pen_up, false);
        }
    }
    params.pen_up_delay_ms = prefs->getDoubleLimited(k_pen_up_delay, 0.0, 0.0, 5000.0);
    params.pen_down_delay_ms = prefs->getDoubleLimited(k_pen_down_delay, 0.0, 0.0, 5000.0);
    auto const legacy_lead_enabled = prefs->getBool(k_lead_in_out, false);
    auto const legacy_lead_distance_mm = prefs->getDoubleLimited(k_lead_in_out_dist, 0.0, 0.0, 1000.0);
    params.enable_path_lead_in = prefs->getBool(k_lead_in, legacy_lead_enabled);
    params.lead_in_distance_mm = prefs->getDoubleLimited(k_lead_in_dist, legacy_lead_distance_mm, 0.0, 1000.0);
    params.enable_path_lead_out = prefs->getBool(k_lead_out, legacy_lead_enabled);
    params.lead_out_distance_mm = prefs->getDoubleLimited(k_lead_out_dist, legacy_lead_distance_mm, 0.0, 1000.0);
    params.long_pen_up_mm = prefs->getDoubleLimited(k_long_pen_up_mm, 10.0, -1000.0, 1000.0);
    params.long_move_distance_mm = prefs->getDoubleLimited(k_long_move_dist, 20.0, 0.0, 100000.0);
    params.enable_near_connect = prefs->getBool(k_near_connect, false);
    params.near_connect_distance_mm = prefs->getDoubleLimited(k_near_connect_dist, 0.3, 0.0, 1000.0);
    params.enable_sparse_stroke_sampling = prefs->getBool(k_sparse_sampling, false);
    params.sparse_keep_every = prefs->getIntLimited(k_sparse_keep_every, 1, 1, 64);
    params.sparse_sampling_strategy = sparse_sampling_strategy_from_pref(prefs->getString(k_sparse_strategy, "legacy"));
    params.optimize_stroke_order = prefs->getBool(k_opt, true);
    params.optimize_stroke_direction = prefs->getBool(k_opt_dir, true);
    params.contour_to_hatch = prefs->getBool(k_hatch, false);
    params.hatch_spacing_mm = prefs->getDoubleLimited(k_hatch_spacing, 1.0, 0.05, 100.0);
    params.hatch_angle_deg = prefs->getDoubleLimited(k_hatch_angle, 0.0, -180.0, 180.0);
    params.hatch_cross = prefs->getBool(k_hatch_cross, false);
    params.hatch_inset_enable = prefs->getBool(k_hatch_inset_enable, false);
    params.hatch_inset_mm = prefs->getDoubleLimited(k_hatch_inset_mm, 0.1, 0.0, 100.0);
    params.hatch_angle_increment_enable = prefs->getBool(k_hatch_angle_increment_enable, false);
    params.hatch_angle_increment_deg = prefs->getDoubleLimited(k_hatch_angle_increment_deg, 5.0, -180.0, 180.0);
    params.flip_y_canvas = prefs->getBool(k_flip, false);
    params.swap_xy = prefs->getBool(k_swap_xy, false);
    params.invert_x = prefs->getBool(k_invert_x, false);
    params.invert_y = prefs->getBool(k_invert_y, false);
    params.align_content_min_to_origin = prefs->getBool(k_align, false);
    params.clip_to_machine_bed = prefs->getBool(k_pref_clip_bed, true);
    params.machine_bed_width_mm = prefs->getDoubleLimited(k_bw, 210.0, 1.0, 2000.0);
    params.machine_bed_depth_mm = prefs->getDoubleLimited(k_bd, 297.0, 1.0, 2000.0);
    bool const auto_pause_between_layers = prefs->getBool(k_autopause, false);
    bool const manual_pen_change = prefs->getBool(k_manual_pen, false);
    params.pen_change_to_home = prefs->getBool(k_pen_ch_home, true);
    params.pen_change_prompt = prefs->getBool(k_pen_ch_prompt, true);
    bool const layer_tool_change_m6 = prefs->getBool(k_tool_change_m6, false);
    if (layer_tool_change_m6) {
        params.auto_pause_between_layers = true;
        params.manual_pen_change = false;
        params.enable_layer_tool_change_m6 = true;
    } else {
        params.auto_pause_between_layers = auto_pause_between_layers;
        params.manual_pen_change = auto_pause_between_layers && manual_pen_change;
        params.enable_layer_tool_change_m6 = false;
    }
    params.tool_change_use_point = prefs->getBool(k_tool_change_point, false);
    params.tool_change_x_mm = prefs->getDouble(k_tool_change_x);
    params.tool_change_y_mm = prefs->getDouble(k_tool_change_y);
    params.auto_layer_pause_dwell_sec = prefs->getDoubleLimited(k_layer_dwell, 0.0, 0.0, 600.0);
    params.start_gcode = prefs->getString(k_start_gcode, "");
    params.end_gcode = normalize_end_gcode(prefs->getString(k_pref_end_gcode, k_default_end_gcode));
}

static bool collect_preview_doc_strokes(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                                        std::vector<std::vector<Geom::Point>> &strokes_out, std::string &err_out)
{
    strokes_out.clear();
    if (!doc) {
        err_out = _("Invalid document.");
        return false;
    }
    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }

    if (wants_layered_pause(params, ctx)) {
        std::vector<std::vector<std::vector<Geom::Point>>> layers_doc;
        std::vector<int> preview_tool_ids;
        collect_layers_doc_strokes(ctx.desktop, doc, params.flatness, layers_doc, &preview_tool_ids);
        if (layers_doc.size() > 1) {
            if (!params.auto_pause_between_layers && params.enable_layer_tool_change_m6) {
                coalesce_consecutive_same_tool_layers(layers_doc, &preview_tool_ids, nullptr);
            }
            auto const mapper = build_document_mm_mapper(doc);
            double const quantum_doc = 0.001 * mapper.avg_doc_units_per_mm();
            double const spacing_doc = params.hatch_spacing_mm * mapper.avg_doc_units_per_mm();
            double const inset_doc = params.hatch_inset_mm * mapper.avg_doc_units_per_mm();
            for (std::size_t i = 0; i < layers_doc.size(); ++i) {
                auto layer_doc = layers_doc[i];
                if (params.optimize_stroke_order && layer_doc.size() > 1) {
                    reorder_strokes_quantized_for_grbl(layer_doc, params.optimize_stroke_direction, quantum_doc);
                }
                merge_exact_touching_strokes(layer_doc, 1e-9);
                if (params.optimize_stroke_order && layer_doc.size() > 1) {
                    reorder_strokes_nearest_neighbor(layer_doc, params.optimize_stroke_direction);
                }
                if (params.contour_to_hatch) {
                    convert_closed_contours_to_hatch(layer_doc, spacing_doc, params.hatch_angle_deg, params.hatch_cross,
                                                     params.hatch_inset_enable, inset_doc,
                                                     params.hatch_angle_increment_enable, params.hatch_angle_increment_deg);
                }
                for (auto &st : layer_doc) {
                    if (st.size() >= 2) {
                        strokes_out.push_back(std::move(st));
                    }
                }
            }
            if (strokes_out.empty()) {
                err_out = _("No drawable vector paths found (convert objects to paths if needed).");
                return false;
            }
            return true;
        }
    }

    std::vector<std::vector<Geom::Point>> strokes_doc;
    collect_strokes_for_context(ctx, doc, params.flatness, strokes_doc);
    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }
    auto const mapper = build_document_mm_mapper(doc);
    double const quantum_doc = 0.001 * mapper.avg_doc_units_per_mm();
    if (params.optimize_stroke_order && strokes_doc.size() > 1) {
        reorder_strokes_quantized_for_grbl(strokes_doc, params.optimize_stroke_direction, quantum_doc);
    }
    merge_exact_touching_strokes(strokes_doc, 1e-9);
    if (params.optimize_stroke_order && strokes_doc.size() > 1) {
        reorder_strokes_nearest_neighbor(strokes_doc, params.optimize_stroke_direction);
    }
    if (params.contour_to_hatch) {
        double const spacing_doc = params.hatch_spacing_mm * mapper.avg_doc_units_per_mm();
        double const inset_doc = params.hatch_inset_mm * mapper.avg_doc_units_per_mm();
        convert_closed_contours_to_hatch(strokes_doc, spacing_doc, params.hatch_angle_deg, params.hatch_cross,
                                         params.hatch_inset_enable, inset_doc, params.hatch_angle_increment_enable,
                                         params.hatch_angle_increment_deg);
    }
    constexpr std::size_t max_strokes = 200000;
    if (strokes_doc.size() > max_strokes) {
        strokes_doc.resize(max_strokes);
    }
    for (auto &st : strokes_doc) {
        if (st.size() >= 2) {
            strokes_out.push_back(std::move(st));
        }
    }
    if (strokes_out.empty()) {
        err_out = _("No drawable vector paths found (convert objects to paths if needed).");
        return false;
    }
    return true;
}

bool build_grbl_plot_preview_pathvector(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                                        Geom::PathVector &paths_out, std::string &err_out, std::size_t max_strokes,
                                        std::size_t *strokes_included_out, std::size_t *strokes_total_out)
{
    paths_out.clear();
    err_out.clear();
    if (strokes_included_out) {
        *strokes_included_out = 0;
    }
    if (strokes_total_out) {
        *strokes_total_out = 0;
    }
    if (max_strokes == 0) {
        err_out = _("Preview stroke limit is zero.");
        return false;
    }

    std::vector<std::vector<Geom::Point>> strokes;
    if (!collect_preview_doc_strokes(doc, params, ctx, strokes, err_out)) {
        return false;
    }

    std::size_t const total = strokes.size();
    if (strokes_total_out) {
        *strokes_total_out = total;
    }
    std::size_t const limit = std::min(total, max_strokes);
    for (std::size_t i = 0; i < limit; ++i) {
        auto const &st = strokes[i];
        Geom::Path pat;
        pat.start(st[0]);
        for (std::size_t j = 1; j < st.size(); ++j) {
            pat.appendNew<Geom::LineSegment>(st[j]);
        }
        paths_out.push_back(std::move(pat));
    }
    if (strokes_included_out) {
        *strokes_included_out = limit;
    }
    return true;
}

static void flatten_prep_strokes_mm(PreparedPlotMm const &prep, std::vector<std::vector<Geom::Point>> &out)
{
    out.clear();
    if (prep.layered) {
        for (auto const &layer : prep.layers_mm) {
            for (auto const &st : layer) {
                if (st.size() >= 2) {
                    out.push_back(st);
                }
            }
        }
    } else {
        out = prep.flat_mm;
    }
}

static Geom::Point mm_after_plot_mapping_toward_doc(Geom::Point mm, DocumentMmMapper const &mapper, PreparedPlotMm const &meta)
{
    if (meta.preview_align_applied) {
        mm[Geom::X] += meta.preview_align_shift_x_mm;
        mm[Geom::Y] += meta.preview_align_shift_y_mm;
    }
    auto const mapped_bed_width_mm = meta.preview_swap_xy_applied ? meta.preview_machine_bed_depth_mm
                                                                  : meta.preview_machine_bed_width_mm;
    auto const mapped_bed_depth_mm = meta.preview_swap_xy_applied ? meta.preview_machine_bed_width_mm
                                                                  : meta.preview_machine_bed_depth_mm;
    if (meta.preview_invert_x_applied) {
        mm[Geom::X] = mapped_bed_width_mm > 1e-6 ? (mapped_bed_width_mm - mm[Geom::X]) : -mm[Geom::X];
    }
    if (meta.preview_invert_y_applied) {
        mm[Geom::Y] = mapped_bed_depth_mm > 1e-6 ? (mapped_bed_depth_mm - mm[Geom::Y]) : -mm[Geom::Y];
    }
    if (meta.preview_swap_xy_applied) {
        std::swap(mm[Geom::X], mm[Geom::Y]);
    }
    if (meta.preview_flip_y_applied && meta.preview_page_h_mm > 0) {
        mm[Geom::Y] = meta.preview_page_h_mm - mm[Geom::Y];
    }
    return mapper.mm_to_doc(mm);
}

bool build_grbl_plot_machine_preview_pathvector_in_doc_space(SPDocument *doc, GrblExportParams const &params,
                                                             GrblExportContext const &ctx, Geom::PathVector &paths_out,
                                                             std::string &err_out, bool *approximate_due_to_clip_out,
                                                             std::size_t max_strokes, std::size_t *strokes_included_out,
                                                             std::size_t *strokes_total_out)
{
    paths_out.clear();
    err_out.clear();
    if (approximate_due_to_clip_out) {
        *approximate_due_to_clip_out = false;
    }
    if (strokes_included_out) {
        *strokes_included_out = 0;
    }
    if (strokes_total_out) {
        *strokes_total_out = 0;
    }
    if (max_strokes == 0) {
        err_out = _("Preview stroke limit is zero.");
        return false;
    }
    if (!doc) {
        err_out = _("Invalid document.");
        return false;
    }
    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }

    PreparedPlotMm prep;
    if (!fill_prepared_plot_mm(doc, params, ctx, prep, err_out)) {
        return false;
    }
    if (approximate_due_to_clip_out) {
        *approximate_due_to_clip_out = prep.preview_clip_applied;
    }

    std::vector<std::vector<Geom::Point>> strokes_mm;
    flatten_prep_strokes_mm(prep, strokes_mm);
    if (strokes_mm.empty()) {
        err_out = _("Nothing to preview after preparing the plot.");
        return false;
    }

    auto const mapper = build_document_mm_mapper(doc);
    std::size_t const total = strokes_mm.size();
    if (strokes_total_out) {
        *strokes_total_out = total;
    }
    std::size_t const limit = std::min(total, max_strokes);
    for (std::size_t i = 0; i < limit; ++i) {
        auto const &st = strokes_mm[i];
        Geom::Path pat;
        pat.start(mm_after_plot_mapping_toward_doc(st[0], mapper, prep));
        for (std::size_t j = 1; j < st.size(); ++j) {
            pat.appendNew<Geom::LineSegment>(mm_after_plot_mapping_toward_doc(st[j], mapper, prep));
        }
        paths_out.push_back(std::move(pat));
    }
    if (strokes_included_out) {
        *strokes_included_out = limit;
    }
    return true;
}

bool build_grbl_plot_gcode_string(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                                  std::string &gcode_out, std::string &err_out, std::size_t *stroke_count_out,
                                  std::size_t const max_output_bytes, GrblPlotStats *stats_out)
{
    err_out.clear();
    gcode_out.clear();
    if (stroke_count_out) {
        *stroke_count_out = 0;
    }
    if (stats_out) {
        *stats_out = {};
    }
    if (!doc) {
        err_out = _("Invalid document.");
        return false;
    }
    if (ctx.cancel && ctx.cancel->load()) {
        err_out = grbl_error_user_cancelled();
        return false;
    }

    PreparedPlotMm prep;
    if (!fill_prepared_plot_mm(doc, params, ctx, prep, err_out)) {
        return false;
    }

    if (stroke_count_out) {
        *stroke_count_out = prep.count_strokes();
    }
    if (stats_out) {
        fill_grbl_plot_stats_from_prep(prep, params, *stats_out);
    }

    GrblExportContext ctx_preview = ctx;
    ctx_preview.inhibit_interactive_pen_changes = true;
    ctx_preview.on_manual_pen_change_between_layers = {};

    return emit_grbl_program_dispatch(nullptr, &gcode_out, max_output_bytes, prep, params, ctx_preview, err_out);
}

} // namespace Inkscape::Axidraw

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
