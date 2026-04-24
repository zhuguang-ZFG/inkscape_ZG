// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Export visible shape geometry as GRBL G-code over an open serial port.
 */

#include "grbl-export.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iterator>
#include <limits>
#include <list>
#include <regex>
#include <sstream>
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

struct DocumentMmMapper {
    double origin_x_doc = 0.0;
    double origin_y_doc = 0.0;
    double mm_per_doc_x = 0.0;
    double mm_per_doc_y = 0.0;

    [[nodiscard]] bool valid() const
    {
        return mm_per_doc_x > 0.0 && mm_per_doc_y > 0.0;
    }

    [[nodiscard]] Geom::Point doc_to_mm(Geom::Point const &p) const
    {
        return {(p[Geom::X] - origin_x_doc) * mm_per_doc_x,
                (p[Geom::Y] - origin_y_doc) * mm_per_doc_y};
    }

    [[nodiscard]] Geom::Point mm_to_doc(Geom::Point const &p) const
    {
        return {origin_x_doc + p[Geom::X] / mm_per_doc_x,
                origin_y_doc + p[Geom::Y] / mm_per_doc_y};
    }

    [[nodiscard]] double avg_doc_units_per_mm() const
    {
        return 0.5 * ((1.0 / mm_per_doc_x) + (1.0 / mm_per_doc_y));
    }
};

DocumentMmMapper build_document_mm_mapper(SPDocument *doc)
{
    DocumentMmMapper mapper;
    if (!doc) {
        return mapper;
    }

    auto const viewbox = doc->getViewBox();
    auto const page_px = doc->getDimensions();
    double const page_w_mm = page_px[Geom::X] * k_mm_per_px;
    double const page_h_mm = page_px[Geom::Y] * k_mm_per_px;

    if (viewbox.width() > 1e-9 && page_w_mm > 1e-9) {
        mapper.origin_x_doc = viewbox.left();
        mapper.mm_per_doc_x = page_w_mm / viewbox.width();
    }
    if (viewbox.height() > 1e-9 && page_h_mm > 1e-9) {
        mapper.origin_y_doc = viewbox.top();
        mapper.mm_per_doc_y = page_h_mm / viewbox.height();
    }

    if (!mapper.valid()) {
        double const px_to_mm = k_mm_per_px;
        mapper.origin_x_doc = 0.0;
        mapper.origin_y_doc = 0.0;
        mapper.mm_per_doc_x = px_to_mm;
        mapper.mm_per_doc_y = px_to_mm;
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
            Geom::Affine const tf = item->i2doc_affine();
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

static void reorder_strokes_nearest_neighbor(std::vector<std::vector<Geom::Point>> &strokes, bool allow_reverse)
{
    strokes.erase(std::remove_if(strokes.begin(), strokes.end(),
                                 [](std::vector<Geom::Point> const &s) { return s.size() < 2; }),
                  strokes.end());
    if (strokes.size() <= 1) {
        return;
    }
    std::vector<std::vector<Geom::Point>> out;
    out.reserve(strokes.size());
    std::vector<bool> used(strokes.size(), false);
    Geom::Point const origin(0, 0);
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
}

static bool stroke_is_closed(std::vector<Geom::Point> const &stroke, double eps_mm = 0.05)
{
    return stroke.size() >= 4 && Geom::L2(stroke.front() - stroke.back()) <= eps_mm;
}

static std::vector<std::vector<Geom::Point>>
contour_to_hatch_scanlines(std::vector<Geom::Point> const &closed_stroke, double spacing_mm)
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

    double min_y = std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();
    for (auto const &p : poly) {
        min_y = std::min(min_y, p[Geom::Y]);
        max_y = std::max(max_y, p[Geom::Y]);
    }
    if (!std::isfinite(min_y) || !std::isfinite(max_y) || max_y - min_y < 1e-6) {
        return out;
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
            out.push_back(std::move(seg));
            flip_dir = !flip_dir;
        }
    }
    return out;
}

static void convert_closed_contours_to_hatch(std::vector<std::vector<Geom::Point>> &strokes_mm, double spacing_mm)
{
    if (spacing_mm <= 1e-6 || strokes_mm.empty()) {
        return;
    }
    std::vector<std::vector<Geom::Point>> out;
    out.reserve(strokes_mm.size());
    for (auto const &stroke : strokes_mm) {
        if (!stroke_is_closed(stroke)) {
            out.push_back(stroke);
            continue;
        }
        auto hatch = contour_to_hatch_scanlines(stroke, spacing_mm);
        if (hatch.empty()) {
            out.push_back(stroke);
            continue;
        }
        out.insert(out.end(), std::make_move_iterator(hatch.begin()), std::make_move_iterator(hatch.end()));
    }
    strokes_mm = std::move(out);
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
    if (!doc) {
        return 0;
    }
    return doc->getDimensions()[Geom::Y] * k_mm_per_px;
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

static void apply_axis_mapping(std::vector<std::vector<Geom::Point>> &strokes, bool swap_xy, bool invert_x, bool invert_y)
{
    if (!swap_xy && !invert_x && !invert_y) {
        return;
    }
    for (auto &st : strokes) {
        for (auto &p : st) {
            if (swap_xy) {
                std::swap(p[Geom::X], p[Geom::Y]);
            }
            if (invert_x) {
                p[Geom::X] = -p[Geom::X];
            }
            if (invert_y) {
                p[Geom::Y] = -p[Geom::Y];
            }
        }
    }
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

    std::vector<std::vector<Geom::Point>> out;
    out.reserve(strokes.size());

    for (auto &stroke : strokes) {
        if (stroke.size() < 2) {
            continue;
        }
        if (out.empty()) {
            out.push_back(std::move(stroke));
            continue;
        }

        auto &prev = out.back();
        if (prev.size() < 2) {
            prev = std::move(stroke);
            continue;
        }

        double const gap = Geom::L2(stroke.front() - prev.back());
        if (gap <= near_dist_mm) {
            auto it = stroke.begin();
            if (Geom::are_near(prev.back(), stroke.front(), 1e-9)) {
                ++it;
            }
            prev.insert(prev.end(), it, stroke.end());
        } else {
            out.push_back(std::move(stroke));
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

static void sparse_sample_strokes(std::vector<std::vector<Geom::Point>> &strokes, int keep_every)
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

static void sparse_sample_strokes_layers(std::vector<std::vector<std::vector<Geom::Point>>> &layers, int keep_every)
{
    if (keep_every <= 1) {
        return;
    }
    for (auto &layer : layers) {
        sparse_sample_strokes(layer, keep_every);
    }
}

static std::size_t count_custom_gcode_lines(Glib::ustring const &block)
{
    std::size_t count = 0;
    std::istringstream in(block.raw());
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
        ++count;
    }
    return count;
}

} // namespace

struct PreparedPlotMm {
    bool layered = false;
    std::vector<std::vector<Geom::Point>> flat_mm;
    std::vector<std::vector<std::vector<Geom::Point>>> layers_mm;
    std::vector<int> layer_tool_ids;
    std::vector<Glib::ustring> layer_labels;

    /// Set by fill_prepared_plot_mm for inverse “machine mm → document” canvas preview.
    bool preview_flip_y_applied = false;
    double preview_page_h_mm = 0;
    bool preview_align_applied = false;
    double preview_align_shift_x_mm = 0;
    double preview_align_shift_y_mm = 0;
    bool preview_clip_applied = false;

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

static void fill_grbl_plot_lengths_from_prep(PreparedPlotMm const &prep, GrblExportParams const &params,
                                             GrblPlotStats &st)
{
    st.has_length_stats = false;
    st.draw_length_mm = 0.0;
    st.travel_length_mm = 0.0;

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
        consume_layer(prep.flat_mm, st.draw_length_mm, st.travel_length_mm, pos, has_any);
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
                st.travel_length_mm += Geom::L2(origin - resume);
                st.travel_length_mm += Geom::L2(resume - origin);
            } else if (needs_tool_change && params.tool_change_use_point) {
                Geom::Point const change(params.tool_change_x_mm, params.tool_change_y_mm);
                Geom::Point const resume = pos;
                st.travel_length_mm += Geom::L2(change - resume);
                st.travel_length_mm += Geom::L2(resume - change);
            }
            consume_layer(layer, st.draw_length_mm, st.travel_length_mm, pos, has_any);
            first_nonempty_layer = false;
            if (next_tool >= 0) {
                active_tool = next_tool;
            }
        }
    }

    st.has_length_stats = has_any;
}

static void fill_grbl_plot_stats_from_prep(PreparedPlotMm const &prep, GrblExportParams const &params,
                                           GrblPlotStats &st)
{
    st.stroke_count = prep.count_strokes();
    st.layer_count = prep.layered ? prep.layers_mm.size() : (prep.flat_mm.empty() ? 0 : 1);
    st.tool_change_count = 0;
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
                                         bool invert_x, bool invert_y)
{
    for (auto &layer : layers) {
        apply_axis_mapping(layer, swap_xy, invert_x, invert_y);
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
    prep.preview_clip_applied = false;

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
            prep.layered = true;
            prep.layers_mm.resize(layers_doc.size());
            for (std::size_t i = 0; i < layers_doc.size(); ++i) {
                auto layer_doc = layers_doc[i];
                if (params.optimize_stroke_order && layer_doc.size() > 1) {
                    reorder_strokes_nearest_neighbor(layer_doc, params.optimize_stroke_direction);
                }
                strokes_doc_to_mm(layer_doc, mapper, prep.layers_mm[i]);
                if (params.contour_to_hatch) {
                    convert_closed_contours_to_hatch(prep.layers_mm[i], params.hatch_spacing_mm);
                }
            }
            double const page_h = document_page_height_mm(doc);
            if (params.flip_y_canvas) {
                apply_flip_y_to_layers(prep.layers_mm, page_h);
                prep.preview_flip_y_applied = true;
                prep.preview_page_h_mm = page_h;
            }
            apply_axis_mapping_to_layers(prep.layers_mm, params.swap_xy, params.invert_x, params.invert_y);
            if (params.align_content_min_to_origin) {
                double shx = 0;
                double shy = 0;
                if (apply_align_min_to_layers(prep.layers_mm, &shx, &shy)) {
                    prep.preview_align_applied = true;
                    prep.preview_align_shift_x_mm = shx;
                    prep.preview_align_shift_y_mm = shy;
                }
            }
            bool const skip_clip = prep.layers_mm.size() > 1 && params.clip_to_machine_bed;
            if (params.clip_to_machine_bed && params.machine_bed_width_mm > 1e-6 && params.machine_bed_depth_mm > 1e-6
                && !skip_clip) {
                for (auto &layer : prep.layers_mm) {
                    clip_strokes_to_axis_rect(layer, 0, 0, params.machine_bed_width_mm, params.machine_bed_depth_mm);
                }
                prep.preview_clip_applied = true;
            }
            if (params.enable_sparse_stroke_sampling) {
                sparse_sample_strokes_layers(prep.layers_mm, params.sparse_keep_every);
            }
            if (params.enable_near_connect) {
                connect_nearby_strokes_layers(prep.layers_mm, params.near_connect_distance_mm);
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

    if (params.optimize_stroke_order && strokes_doc.size() > 1) {
        reorder_strokes_nearest_neighbor(strokes_doc, params.optimize_stroke_direction);
    }

    constexpr std::size_t max_strokes = 200000;
    if (strokes_doc.size() > max_strokes) {
        strokes_doc.resize(max_strokes);
    }

    if (strokes_doc.empty()) {
        err_out = _("No drawable vector paths found (convert objects to paths if needed).");
        return false;
    }

    strokes_doc_to_mm(strokes_doc, mapper, prep.flat_mm);
    if (params.contour_to_hatch) {
        convert_closed_contours_to_hatch(prep.flat_mm, params.hatch_spacing_mm);
    }
    if (prep.flat_mm.empty()) {
        err_out = _("No drawable vector paths found (convert objects to paths if needed).");
        return false;
    }

    if (params.flip_y_canvas) {
        double const ph = document_page_height_mm(doc);
        apply_flip_y_canvas(prep.flat_mm, ph);
        prep.preview_flip_y_applied = true;
        prep.preview_page_h_mm = ph;
    }
    apply_axis_mapping(prep.flat_mm, params.swap_xy, params.invert_x, params.invert_y);
    if (params.align_content_min_to_origin) {
        double shx = 0;
        double shy = 0;
        if (apply_align_min_to_origin(prep.flat_mm, &shx, &shy)) {
            prep.preview_align_applied = true;
            prep.preview_align_shift_x_mm = shx;
            prep.preview_align_shift_y_mm = shy;
        }
    }
    if (params.clip_to_machine_bed && params.machine_bed_width_mm > 1e-6 && params.machine_bed_depth_mm > 1e-6) {
        clip_strokes_to_axis_rect(prep.flat_mm, 0, 0, params.machine_bed_width_mm, params.machine_bed_depth_mm);
        prep.preview_clip_applied = true;
    }
    if (params.enable_sparse_stroke_sampling) {
        sparse_sample_strokes(prep.flat_mm, params.sparse_keep_every);
    }
    if (params.enable_near_connect) {
        connect_nearby_strokes(prep.flat_mm, params.near_connect_distance_mm);
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
    std::size_t n = 2 + count_custom_gcode_lines(params.start_gcode) + count_custom_gcode_lines(params.end_gcode);
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
    std::size_t n = 2 + count_custom_gcode_lines(params.start_gcode) + count_custom_gcode_lines(params.end_gcode);
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
    std::istringstream in(block.raw());
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
        if (!emit_line_impl(port, gcode_out, max_out_bytes, ctx, err_out, line)) {
            return false;
        }
    }
    return true;
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
        if (!emit_line((use_long_pen_up ? long_pen_up : pen_up).raw())) {
            return false;
        }

        if (!has_prev_end || !Geom::are_near(stroke.front(), prev_end, k_machine_coord_epsilon_mm)) {
            if (!emit_line(format_xy_mm(stroke.front(), "G0", params.feed_travel_mm_min))) {
                return false;
            }
        }

        if (!emit_line(pen_dn.raw())) {
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

    if (!emit_line(pen_up.raw())) {
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
            if (!emit_line((use_long_pen_up ? long_pen_up : pen_up).raw())) {
                return false;
            }
            if (!has_last || !Geom::are_near(stroke.front(), last_mm, k_machine_coord_epsilon_mm)) {
                if (!emit_line(format_xy_mm(stroke.front(), "G0", params.feed_travel_mm_min))) {
                    return false;
                }
            }
            if (!emit_line(pen_dn.raw())) {
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

            if (!emit_line(pen_up.raw())) {
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

    if (!emit_line(pen_up.raw())) {
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
    constexpr auto k_flat = "/options/grbl/flatness";
    constexpr auto k_fd = "/options/grbl/feed-draw-mmmin";
    constexpr auto k_ft = "/options/grbl/feed-travel-mmmin";
    constexpr auto k_pen_up = "/options/grbl/pen-up-cmd";
    constexpr auto k_pen_dn = "/options/grbl/pen-down-cmd";
    constexpr auto k_pen_ctl = "/options/grbl/pen-control";
    constexpr auto k_long_pen_up = "/options/grbl/enable-long-pen-up";
    constexpr auto k_long_pen_up_mm = "/options/grbl/long-pen-up-mm";
    constexpr auto k_long_move_dist = "/options/grbl/long-move-dist-mm";
    constexpr auto k_near_connect = "/options/grbl/enable-near-connect";
    constexpr auto k_near_connect_dist = "/options/grbl/near-connect-distance-mm";
    constexpr auto k_sparse_sampling = "/options/grbl/enable-sparse-stroke-sampling";
    constexpr auto k_sparse_keep_every = "/options/grbl/sparse-keep-every";
    constexpr auto k_opt = "/options/grbl/optimize-stroke-order";
    constexpr auto k_opt_dir = "/options/grbl/optimize-stroke-direction";
    constexpr auto k_hatch = "/options/grbl/contour-to-hatch";
    constexpr auto k_hatch_spacing = "/options/grbl/hatch-spacing-mm";
    constexpr auto k_flip = "/options/grbl/flip-y-canvas";
    constexpr auto k_swap_xy = "/options/grbl/swap-xy";
    constexpr auto k_invert_x = "/options/grbl/invert-x";
    constexpr auto k_invert_y = "/options/grbl/invert-y";
    constexpr auto k_align = "/options/grbl/align-content-min";
    constexpr auto k_clip = "/options/grbl/clip-to-machine-bed";
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
    constexpr auto k_end_gcode = "/options/grbl/end-gcode";

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
            params.pen_up_cmd = prefs->getString(k_pen_up, "G1 Z0 F3000");
            params.pen_down_cmd = prefs->getString(k_pen_dn, "G1 Z5 F3000");
            params.enable_long_pen_up = prefs->getBool(k_long_pen_up, false);
        }
    }
    params.long_pen_up_mm = prefs->getDoubleLimited(k_long_pen_up_mm, 10.0, -1000.0, 1000.0);
    params.long_move_distance_mm = prefs->getDoubleLimited(k_long_move_dist, 20.0, 0.0, 100000.0);
    params.enable_near_connect = prefs->getBool(k_near_connect, false);
    params.near_connect_distance_mm = prefs->getDoubleLimited(k_near_connect_dist, 0.3, 0.0, 1000.0);
    params.enable_sparse_stroke_sampling = prefs->getBool(k_sparse_sampling, false);
    params.sparse_keep_every = prefs->getIntLimited(k_sparse_keep_every, 1, 1, 64);
    params.optimize_stroke_order = prefs->getBool(k_opt, true);
    params.optimize_stroke_direction = prefs->getBool(k_opt_dir, true);
    params.contour_to_hatch = prefs->getBool(k_hatch, false);
    params.hatch_spacing_mm = prefs->getDoubleLimited(k_hatch_spacing, 1.0, 0.05, 100.0);
    params.flip_y_canvas = prefs->getBool(k_flip, false);
    params.swap_xy = prefs->getBool(k_swap_xy, false);
    params.invert_x = prefs->getBool(k_invert_x, false);
    params.invert_y = prefs->getBool(k_invert_y, false);
    params.align_content_min_to_origin = prefs->getBool(k_align, false);
    params.clip_to_machine_bed = prefs->getBool(k_clip, false);
    params.machine_bed_width_mm = prefs->getDoubleLimited(k_bw, 300.0, 1.0, 2000.0);
    params.machine_bed_depth_mm = prefs->getDoubleLimited(k_bd, 200.0, 1.0, 2000.0);
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
    params.end_gcode = prefs->getString(k_end_gcode, "");
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
        collect_layers_doc_strokes(ctx.desktop, doc, params.flatness, layers_doc);
        if (layers_doc.size() > 1) {
            for (std::size_t i = 0; i < layers_doc.size(); ++i) {
                auto layer_doc = layers_doc[i];
                if (params.optimize_stroke_order && layer_doc.size() > 1) {
                    reorder_strokes_nearest_neighbor(layer_doc, params.optimize_stroke_direction);
                }
                if (params.contour_to_hatch) {
                    auto const mapper = build_document_mm_mapper(doc);
                    double const spacing_doc = params.hatch_spacing_mm * mapper.avg_doc_units_per_mm();
                    convert_closed_contours_to_hatch(layer_doc, spacing_doc);
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
    if (params.optimize_stroke_order && strokes_doc.size() > 1) {
        reorder_strokes_nearest_neighbor(strokes_doc, params.optimize_stroke_direction);
    }
    if (params.contour_to_hatch) {
        auto const mapper = build_document_mm_mapper(doc);
        double const spacing_doc = params.hatch_spacing_mm * mapper.avg_doc_units_per_mm();
        convert_closed_contours_to_hatch(strokes_doc, spacing_doc);
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
