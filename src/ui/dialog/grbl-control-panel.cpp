// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Serial GRBL / axis jog control (dockable dialog).
 *
 * \par Reference (decompiled Java only)
 * From kxnx `tools/dump_agent/windows_reverse/decompiled_java/com/kvenjoy/drawsoft/lib/remote/pck/PrintPck.java`
 * and `com/kvenjoy/drawsoft/lib/e/f.java` (`a(String s)` is true when `s == null || s.length() == 0`):
 *   - `public String singleGcode;` 鈥?in `write(b)`, if `f.a(this.uuid)` (uuid empty) is true, the
 *     code writes byte `0` then the UTF-8 bytes of `singleGcode` (length-prefixed via `b2.b` / `b2.a`);
 *   - if `!f.a(this.uuid)` (uuid not empty), it writes byte `1`, then `uuid` and `public int repeat;`
 *     and returns (no `singleGcode` in that branch in the decompiled source).
 * This dialog does not implement that packet format: it uses a local `SerialPort` and `grbl_send_line`
 * in C++ only; line splitting and comments are handled below without claiming parity with the Java host.
 */

#include "grbl-control-panel.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#include <glib.h>
#include <giomm/liststore.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/textbuffer.h>
#include <gtkmm/window.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/ustring.h>

#include <2geom/pathvector.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/serial-port-scan.h"
#include "axidraw/device/serial-port.h"
#include "axidraw/device/tcp-port.h"
#include "axidraw/pipeline/grbl-export.h"
#include "desktop.h"
#include "display/control/canvas-item-text.h"
#include "document.h"
#include "document-undo.h"
#include "io/sys.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "preferences.h"
#include "style-enums.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog-run.h"
#include "ui/pack.h"
#include "ui/widget/canvas.h"
#include "util/scope_exit.h"
#include "util/units.h"
#include "xml/node.h"

using Inkscape::Axidraw::grbl_begin_plot_waits;
using Inkscape::Axidraw::grbl_end_plot_waits;
using Inkscape::Axidraw::grbl_error_user_cancelled;
using Inkscape::Axidraw::build_grbl_plot_gcode_string;
using Inkscape::Axidraw::build_grbl_plot_machine_preview_pathvector_in_doc_space;
using Inkscape::Axidraw::build_grbl_plot_preview_pathvector;
using Inkscape::Axidraw::GrblPlotStats;
using Inkscape::Axidraw::analyze_grbl_plot;
using Inkscape::Axidraw::export_paths_to_grbl;
using Inkscape::Axidraw::grbl_export_params_from_preferences;
using Inkscape::Axidraw::grbl_send_line;
using Inkscape::Axidraw::SerialPort;
using Inkscape::CanvasItemBpath;
using Inkscape::CanvasItemText;
using Inkscape::choose_file_open;
using Inkscape::choose_file_save;

namespace {

void append_axis_arrow(Geom::PathVector &paths, Geom::Point const &from, Geom::Point const &to, double head_len, double head_width)
{
    Geom::Path shaft(from);
    shaft.appendNew<Geom::LineSegment>(to);
    paths.push_back(shaft);
    auto const dx = to[Geom::X] - from[Geom::X];
    auto const dy = to[Geom::Y] - from[Geom::Y];
    auto const len = std::hypot(dx, dy);
    if (len > 1e-6) {
        Geom::Point const dir(dx / len, dy / len);
        Geom::Point const normal(-dir[Geom::Y], dir[Geom::X]);
        Geom::Point const base = to - dir * head_len;
        Geom::Path left(base + normal * head_width);
        left.appendNew<Geom::LineSegment>(to);
        paths.push_back(left);
        Geom::Path right(base - normal * head_width);
        right.appendNew<Geom::LineSegment>(to);
        paths.push_back(right);
    }
}

Geom::Point machine_axis_direction(bool swap_xy, bool invert_x, bool invert_y, bool x_axis)
{
    if (!swap_xy) {
        return x_axis
            ? Geom::Point(invert_x ? -1.0 : 1.0, 0.0)
            : Geom::Point(0.0, invert_y ? -1.0 : 1.0);
    }
    return x_axis
        ? Geom::Point(0.0, invert_x ? -1.0 : 1.0)
        : Geom::Point(invert_y ? -1.0 : 1.0, 0.0);
}

Glib::ustring make_preview_summary(bool machine_space, std::size_t included, std::size_t total, bool clip_approx);

Glib::ustring format_duration_compact(double seconds)
{
    if (!(seconds > 0.0)) {
        return _("未估算");
    }
    auto const rounded = static_cast<long long>(std::llround(seconds));
    long long const hours = rounded / 3600;
    long long const minutes = (rounded % 3600) / 60;
    long long const secs = rounded % 60;
    std::ostringstream out;
    if (hours > 0) {
        out << hours << _("小时");
    }
    if (minutes > 0 || hours > 0) {
        out << minutes << _("分");
    }
    out << secs << _("秒");
    return out.str();
}

void trim_in_place(std::string &s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    auto it = s.begin();
    while (it != s.end() && (*it == ' ' || *it == '\t')) {
        ++it;
    }
    s.erase(s.begin(), it);
}

/// Skip empty, `;` comments, and parenthesis-only comment lines; keep inline `(鈥?` on G-code.
bool should_skip_gcode_line(std::string const &s)
{
    if (s.empty()) {
        return true;
    }
    if (s[0] == ';') {
        return true;
    }
    if (s[0] == '(') {
        auto const end = s.find(')');
        if (end != std::string::npos && end + 1 == s.size()) {
            return true;
        }
    }
    return false;
}

std::string detect_radio_mode_from_reply(std::string reply)
{
    for (auto &c : reply) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    auto has_token = [&](char const *tok) {
        std::string const t(tok);
        auto p = reply.find(t);
        if (p == std::string::npos) {
            return false;
        }
        auto boundary = [&](std::size_t idx) {
            if (idx >= reply.size()) {
                return true;
            }
            char const ch = reply[idx];
            return !(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_');
        };
        bool const left_ok = (p == 0) ? true : boundary(p - 1);
        bool const right_ok = boundary(p + t.size());
        return left_ok && right_ok;
    };
    if (has_token("STA")) return "STA";
    if (has_token("AP")) return "AP";
    if (has_token("BT")) return "BT";
    if (has_token("OFF") || has_token("NONE")) return "OFF";
    return {};
}

constexpr std::size_t k_max_gcode_stream_lines = 200000;

/// Counts lines that `on_send_gcode` would send (same skip rules). Returns @c k_max_gcode_stream_lines + 1 if more
/// than that many executable lines exist.
std::size_t count_executable_gcode_lines(std::string const &text)
{
    std::size_t n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        trim_in_place(line);
        if (should_skip_gcode_line(line)) {
            continue;
        }
        ++n;
        if (n > k_max_gcode_stream_lines) {
            return k_max_gcode_stream_lines + 1;
        }
    }
    return n;
}

constexpr int k_gcode_send_progress_min_interval_ms = 350;
constexpr std::size_t k_gcode_send_progress_line_stride = 80;

constexpr auto k_pref_device = "/options/grbl/serial-device";
constexpr auto k_pref_baud = "/options/grbl/baud";
constexpr auto k_pref_travel = "/options/grbl/feed-travel-mmmin";
constexpr auto k_pref_pen_control = "/options/grbl/pen-control";
constexpr auto k_pref_pen_up = "/options/grbl/pen-up-cmd";
constexpr auto k_pref_pen_down = "/options/grbl/pen-down-cmd";
constexpr auto k_pref_limit_layer = "/options/grbl/limit-to-current-layer";
constexpr auto k_pref_sync_page_to_bed = "/options/grbl/sync-page-to-bed-on-firmware-read";
constexpr auto k_pref_net_host = "/options/grbl/net-host";
constexpr auto k_pref_net_port = "/options/grbl/net-port";
constexpr auto k_pref_swap_xy = "/options/grbl/swap-xy";
constexpr auto k_pref_invert_x = "/options/grbl/invert-x";
constexpr auto k_pref_invert_y = "/options/grbl/invert-y";
constexpr auto k_pref_flip_y = "/options/grbl/flip-y-canvas";
constexpr auto k_pref_align_origin = "/options/grbl/align-content-min";
constexpr auto k_pref_clip_bed = "/options/grbl/clip-to-machine-bed";
constexpr auto k_pref_bed_width = "/options/grbl/machine-bed-width-mm";
constexpr auto k_pref_bed_depth = "/options/grbl/machine-bed-depth-mm";
constexpr auto k_pref_long_pen_up = "/options/grbl/enable-long-pen-up";
constexpr auto k_pref_long_pen_up_mm = "/options/grbl/long-pen-up-mm";
constexpr auto k_pref_long_move_dist = "/options/grbl/long-move-dist-mm";
constexpr auto k_pref_near_connect = "/options/grbl/enable-near-connect";
constexpr auto k_pref_near_connect_dist = "/options/grbl/near-connect-distance-mm";
constexpr auto k_pref_sparse_sampling = "/options/grbl/enable-sparse-stroke-sampling";
constexpr auto k_pref_sparse_keep_every = "/options/grbl/sparse-keep-every";
constexpr auto k_pref_auto_pause_between_layers = "/options/grbl/auto-pause-between-layers";
constexpr auto k_pref_manual_pen_change = "/options/grbl/manual-pen-change";
constexpr auto k_pref_pen_change_to_home = "/options/grbl/pen-change-to-home";
constexpr auto k_pref_pen_change_prompt = "/options/grbl/pen-change-prompt";
constexpr auto k_pref_tool_change_m6 = "/options/grbl/enable-layer-tool-change-m6";
constexpr auto k_pref_tool_change_point = "/options/grbl/tool-change-use-point";
constexpr auto k_pref_tool_change_x = "/options/grbl/tool-change-x-mm";
constexpr auto k_pref_tool_change_y = "/options/grbl/tool-change-y-mm";
constexpr auto k_pref_start_gcode = "/options/grbl/start-gcode";
constexpr auto k_pref_end_gcode = "/options/grbl/end-gcode";

Inkscape::Axidraw::GrblExportParams make_export_params_from_preferences()
{
    auto *prefs = Inkscape::Preferences::get();
    Inkscape::Axidraw::GrblExportParams params;
    grbl_export_params_from_preferences(prefs, params);
    return params;
}

Inkscape::Axidraw::GrblExportContext make_export_context(Inkscape::UI::Dialog::GrblControlPanel &panel, SPDesktop *desktop)
{
    auto *prefs = Inkscape::Preferences::get();
    Inkscape::Axidraw::GrblExportContext ctx;
    ctx.desktop = desktop;
    ctx.selection = desktop ? desktop->getSelection() : nullptr;
    ctx.use_current_layer_without_selection = prefs->getBool(k_pref_limit_layer, false);
    ctx.cancel = nullptr;
    return ctx;
}

bool get_document_content_bounds(SPDocument *doc, Geom::Rect &bounds, Glib::ustring &error,
                                 Glib::ustring const &empty_message)
{
    if (!doc) {
        error = _("没有活动文档。");
        return false;
    }
    auto bounds_opt = doc->preferredBounds();
    if (!bounds_opt) {
        error = empty_message;
        return false;
    }
    bounds = *bounds_opt;
    if (!(bounds.width() > 1e-9) || !(bounds.height() > 1e-9)) {
        error = _("当前文档没有有效的绘图范围。");
        return false;
    }
    return true;
}

bool get_bed_size_in_document_units(SPDocument *doc, double bed_width_mm, double bed_height_mm,
                                    double &bed_w_doc, double &bed_h_doc)
{
    if (!doc) {
        return false;
    }
    auto const &unit_table = Inkscape::Util::UnitTable::get();
    auto *doc_unit = doc->getDisplayUnit();
    if (!doc_unit) {
        doc_unit = unit_table.getUnit("px");
    }
    auto *mm = unit_table.getUnit("mm");
    bed_w_doc = Inkscape::Util::Quantity::convert(bed_width_mm, mm, doc_unit);
    bed_h_doc = Inkscape::Util::Quantity::convert(bed_height_mm, mm, doc_unit);
    return bed_w_doc > 1e-9 && bed_h_doc > 1e-9;
}

struct LayoutScaleMetrics
{
    double content_w_doc = 0.0;
    double content_h_doc = 0.0;
    double content_w_mm = 0.0;
    double content_h_mm = 0.0;
    double bed_w_mm = 0.0;
    double bed_h_mm = 0.0;
    double fit_scale = 0.0;
    double fill_x_pct = 0.0;
    double fill_y_pct = 0.0;
    bool fits_without_scaling = false;
};

bool get_layout_scale_metrics_from_bounds_mm(double content_w_mm, double content_h_mm, double bed_width_mm,
                                             double bed_height_mm, LayoutScaleMetrics &metrics, Glib::ustring &error)
{
    if (!(content_w_mm > 1e-9) || !(content_h_mm > 1e-9)) {
        error = _("当前文档没有有效的绘图范围。");
        return false;
    }
    if (!(bed_width_mm > 1e-9) || !(bed_height_mm > 1e-9)) {
        error = _("机器行程无效，请先同步或设置床面宽度/深度。");
        return false;
    }

    metrics.content_w_mm = content_w_mm;
    metrics.content_h_mm = content_h_mm;
    metrics.bed_w_mm = bed_width_mm;
    metrics.bed_h_mm = bed_height_mm;
    metrics.fit_scale = std::min(bed_width_mm / content_w_mm, bed_height_mm / content_h_mm);
    metrics.fill_x_pct = (content_w_mm / bed_width_mm) * 100.0;
    metrics.fill_y_pct = (content_h_mm / bed_height_mm) * 100.0;
    metrics.fits_without_scaling = metrics.fit_scale >= 0.999999;
    return true;
}

bool get_layout_scale_metrics(SPDocument *doc, double bed_width_mm, double bed_height_mm, LayoutScaleMetrics &metrics,
                              Glib::ustring &error, Glib::ustring const &empty_message)
{
    Geom::Rect bounds;
    if (!get_document_content_bounds(doc, bounds, error, empty_message)) {
        return false;
    }

    double bed_w_doc = 0.0;
    double bed_h_doc = 0.0;
    if (!get_bed_size_in_document_units(doc, bed_width_mm, bed_height_mm, bed_w_doc, bed_h_doc)) {
        error = _("机器行程无效，请先同步或设置床面宽度/深度。");
        return false;
    }

    auto const &unit_table = Inkscape::Util::UnitTable::get();
    auto *doc_unit = doc->getDisplayUnit();
    if (!doc_unit) {
        doc_unit = unit_table.getUnit("px");
    }
    auto *mm = unit_table.getUnit("mm");

    metrics.content_w_doc = bounds.width();
    metrics.content_h_doc = bounds.height();
    return get_layout_scale_metrics_from_bounds_mm(Inkscape::Util::Quantity::convert(metrics.content_w_doc, doc_unit, mm),
                                                   Inkscape::Util::Quantity::convert(metrics.content_h_doc, doc_unit, mm),
                                                   bed_width_mm, bed_height_mm, metrics, error);
}

void setup_summary_label(Gtk::Label &label, Glib::ustring const &initial_markup, int margin_top, int margin_bottom)
{
    label.set_halign(Gtk::Align::START);
    label.set_wrap(true);
    label.set_use_markup(true);
    label.set_selectable(true);
    label.set_xalign(0.0f);
    label.add_css_class("monospace");
    label.set_margin_top(margin_top);
    label.set_margin_bottom(margin_bottom);
    label.set_markup(initial_markup);
}

bool get_document_bounds_and_bed(SPDocument *doc, double bed_width_mm, double bed_height_mm, Geom::Rect &bounds,
                                 double &bed_w_doc, double &bed_h_doc, Glib::ustring &error,
                                 Glib::ustring const &empty_message)
{
    if (!get_document_content_bounds(doc, bounds, error, empty_message)) {
        return false;
    }
    if (!get_bed_size_in_document_units(doc, bed_width_mm, bed_height_mm, bed_w_doc, bed_h_doc)) {
        error = _("机器行程无效，请先同步或设置床面宽度/深度。");
        return false;
    }
    return true;
}

void set_markup_message(Gtk::Label &label, Glib::ustring const &title, Glib::ustring const &message)
{
    label.set_markup(Glib::ustring::compose("<b>%1</b>\n%2", title, Glib::Markup::escape_text(message)));
}

void set_layout_scale_summary_from_stats(Gtk::Label &label, Inkscape::Axidraw::GrblPlotStats const &stats,
                                         double bed_width_mm, double bed_height_mm)
{
    LayoutScaleMetrics metrics;
    Glib::ustring error;
    if (!stats.has_bounds_mm ||
        !get_layout_scale_metrics_from_bounds_mm(stats.max_x_mm - stats.min_x_mm, stats.max_y_mm - stats.min_y_mm,
                                                 bed_width_mm, bed_height_mm, metrics, error)) {
        set_markup_message(label, _("当前缩放"), error);
        return;
    }

    std::ostringstream out;
    out << "<b>当前缩放</b>\n";
    out << _("图稿尺寸：") << std::fixed << std::setprecision(1) << metrics.content_w_mm << " x "
        << metrics.content_h_mm << " mm";
    out << "\n" << _("机器行程占用：X ") << std::fixed << std::setprecision(1) << metrics.fill_x_pct << "% / Y "
        << metrics.fill_y_pct << "%";
    out << "\n" << _("一键适配后比例：");
    if (metrics.fits_without_scaling) {
        double const enlarge_pct = metrics.fit_scale * 100.0;
        out << _("当前已在行程内");
        if (enlarge_pct > 100.1) {
            out << "（" << _("若放大到铺满可达") << " " << std::fixed << std::setprecision(1) << enlarge_pct
                << "%，当前按钮默认不放大）";
        }
    } else {
        out << std::fixed << std::setprecision(1) << (metrics.fit_scale * 100.0) << "%";
    }
    label.set_markup(out.str());
}

bool analyze_plot_for_feedback(Inkscape::UI::Dialog::GrblControlPanel &panel, SPDocument *doc, SPDesktop *desktop,
                               Inkscape::Axidraw::GrblPlotStats &stats, std::string &err)
{
    auto params = make_export_params_from_preferences();
    auto ctx = make_export_context(panel, desktop);
    return analyze_grbl_plot(doc, params, ctx, stats, err);
}

void set_no_active_document_summaries(Gtk::Label &job_summary, Gtk::Label &layout_scale_summary)
{
    job_summary.set_markup(_("<b>任务概览</b>\n暂无活动文档。"));
    layout_scale_summary.set_markup(_("<b>当前缩放</b>\n暂无活动文档。"));
}

void set_analysis_error_summaries(Gtk::Label &job_summary, Gtk::Label &layout_scale_summary, std::string const &err)
{
    auto const job_message = err.empty() ? _("当前无法估算任务信息。") : Glib::ustring(err);
    auto const scale_message = err.empty() ? _("当前无法估算缩放信息。") : Glib::ustring(err);
    set_markup_message(job_summary, _("任务概览"), job_message);
    set_markup_message(layout_scale_summary, _("当前缩放"), scale_message);
}

Geom::PathVector transform_pathvector_to_desktop(Geom::PathVector const &paths, Geom::Affine const &affine)
{
    Geom::PathVector transformed;
    for (auto const &path : paths) {
        transformed.push_back(path * affine);
    }
    return transformed;
}

void configure_preview_overlay(CanvasItemBpath &overlay, uint32_t const stroke, double const stroke_width)
{
    overlay.set_stroke(stroke);
    overlay.set_fill(0x00000000, SP_WIND_RULE_NONZERO);
    overlay.set_stroke_width(stroke_width);
    overlay.set_visible(true);
}

Glib::ustring build_preview_status_note(bool const machine_space, std::size_t const included, std::size_t const total,
                                        bool const clip_approx)
{
    if (clip_approx) {
        return make_preview_summary(machine_space, included, total, true);
    }
    if (total > included) {
        return make_preview_summary(machine_space, included, total, false);
    }
    return {};
}

CanvasItemPtr<CanvasItemText> make_preview_axis_label(SPDesktop *desktop, Geom::Point const &pos, Glib::ustring const &text,
                                                      uint32_t const bg)
{
    auto label = make_canvasitem<CanvasItemText>(desktop->getCanvasTemp(), pos, text);
    label->set_fontsize(11.0);
    label->set_background(bg);
    label->set_border(4.0);
    label->set_visible(true);
    return label;
}

Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> create_gcode_file_filters()
{
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto gcode = Gtk::FileFilter::create();
    gcode->set_name(_("G-code 文件"));
    for (auto const *suffix : {"nc", "gcode", "tap", "cnc", "txt"}) {
        gcode->add_suffix(suffix);
    }
    filters->append(gcode);

    auto all = Gtk::FileFilter::create();
    all->set_name(_("所有文件"));
    all->add_pattern("*");
    filters->append(all);
    return filters;
}

Glib::ustring make_preview_build_error(std::string const &err, bool machine_space)
{
    if (!err.empty()) {
        return Glib::ustring(err);
    }
    return machine_space ? Glib::ustring(_("无法生成机器空间预览。")) : Glib::ustring(_("无法生成文档空间预览。"));
}

Glib::ustring make_preview_summary(bool machine_space, std::size_t included, std::size_t total, bool clip_approx)
{
    if (!machine_space) {
        return Glib::ustring::compose(
            _("文档空间预览仅显示了 %1 / %2 段路径；如果图稿过于复杂，请简化图稿或关闭该预览。"),
            static_cast<guint64>(included), static_cast<guint64>(total));
    }
    if (clip_approx && total > included) {
        return Glib::ustring::compose(
            _("机器空间预览仅显示了 %1 / %2 段路径；被机器床面裁切的部分会以近似方式显示。"),
            static_cast<guint64>(included), static_cast<guint64>(total));
    }
    if (clip_approx) {
        return _("机器空间预览中，被机器床面裁切的部分会以近似方式显示。");
    }
    return Glib::ustring::compose(
        _("机器空间预览仅显示了 %1 / %2 段路径；如果图稿过于复杂，请简化图稿以查看全部结果。"),
        static_cast<guint64>(included), static_cast<guint64>(total));
}

Glib::ustring make_fill_gcode_status(std::size_t strokes, Inkscape::Axidraw::GrblPlotStats const &stats)
{
    if (stats.has_bounds_mm) {
        std::ostringstream wxh;
        wxh << std::fixed << std::setprecision(1) << (stats.max_x_mm - stats.min_x_mm) << " x "
            << (stats.max_y_mm - stats.min_y_mm);
        if (stats.has_length_stats && (stats.draw_length_mm + stats.travel_length_mm) > 1e-9) {
            double const total = stats.draw_length_mm + stats.travel_length_mm;
            double const air = (stats.travel_length_mm / total) * 100.0;
            std::ostringstream lengths;
            lengths << std::fixed << std::setprecision(1) << stats.draw_length_mm << " / " << stats.travel_length_mm;
            std::ostringstream ratio;
            ratio << std::fixed << std::setprecision(1) << air;
            return Glib::ustring::compose(
                _("编辑器已填入 %1 条笔画，对应工作区域约 %2 mm，绘制/空走长度 %3 mm，空走占比 %4%%。请先检查，如有需要可“另存为 G-code”，然后再“发送到机器”。"),
                static_cast<guint64>(strokes), Glib::ustring(wxh.str()), Glib::ustring(lengths.str()),
                Glib::ustring(ratio.str()));
        }
        return Glib::ustring::compose(
            _("编辑器已填入 %1 条笔画，对应工作区域约 %2 mm（按首选项换算后的机器坐标）。请先检查，如有需要可“另存为 G-code”，然后再“发送到机器”。"),
            static_cast<guint64>(strokes), Glib::ustring(wxh.str()));
    }
    return Glib::ustring::compose(_("编辑器已为 %1 条笔画生成 G-code。请先检查内容，确认后再“发送到机器”。"),
                                   static_cast<guint64>(strokes));
}

Glib::ustring make_direct_send_progress_status(std::size_t stroke_done, std::size_t stroke_total, std::size_t lines_sent,
                                               std::size_t lines_est)
{
    if (lines_est > 0 && lines_sent > 0 && stroke_total > 0 && stroke_done > 0) {
        return Glib::ustring::compose(_("正在发送：第 %1 / %2 条笔画，第 %3 / ~%4 行..."),
                                      static_cast<guint64>(stroke_done), static_cast<guint64>(stroke_total),
                                      static_cast<guint64>(lines_sent), static_cast<guint64>(lines_est));
    }
    if (lines_est > 0 && lines_sent > 0) {
        return Glib::ustring::compose(_("正在发送：第 %1 / ~%2 行..."), static_cast<guint64>(lines_sent),
                                      static_cast<guint64>(lines_est));
    }
    if (stroke_total > 0 && stroke_done > 0) {
        return Glib::ustring::compose(_("正在发送：第 %1 / %2 条笔画..."), static_cast<guint64>(stroke_done),
                                      static_cast<guint64>(stroke_total));
    }
    return {};
}

Glib::ustring make_direct_send_done_status(std::size_t strokes, Inkscape::Axidraw::GrblPlotStats const &stats)
{
    if (stats.has_length_stats && (stats.draw_length_mm + stats.travel_length_mm) > 1e-9) {
        double const total = stats.draw_length_mm + stats.travel_length_mm;
        double const air = (stats.travel_length_mm / total) * 100.0;
        std::ostringstream ratio;
        ratio << std::fixed << std::setprecision(1) << air;
        return Glib::ustring::compose(_("图稿直发完成：共发送 %1 条笔画，空走占比约 %2%%。"),
                                      static_cast<guint64>(strokes), Glib::ustring(ratio.str()));
    }
    return Glib::ustring::compose(_("图稿直发完成：共发送 %1 条笔画。"), static_cast<guint64>(strokes));
}
/// Last folder for G-code save/open dialogs in this panel.
constexpr auto k_pref_save_gcode_dir = "/dialogs/grblcontrol/save_gcode_dir";
constexpr std::size_t k_max_gcode_editor_bytes = 32u * 1024u * 1024u;

constexpr auto k_tool_change_mode_none = "none";
constexpr auto k_tool_change_mode_manual = "manual";
constexpr auto k_tool_change_mode_m6 = "m6";

struct GrblFirmwareSnapshot {
    bool has_direction_mask = false;
    int direction_mask = 0;
    bool has_x_travel = false;
    double x_travel_mm = 0;
    bool has_y_travel = false;
    double y_travel_mm = 0;
    Glib::ustring display_text;
};

bool parse_grbl_setting_line(std::string const &line, int &code_out, std::string &value_out)
{
    if (line.size() < 4 || line[0] != '$') {
        return false;
    }
    auto const eq = line.find('=');
    if (eq == std::string::npos || eq <= 1) {
        return false;
    }
    try {
        code_out = std::stoi(line.substr(1, eq - 1));
    } catch (...) {
        return false;
    }
    value_out = line.substr(eq + 1);
    trim_in_place(value_out);
    return true;
}

bool parse_double_c(std::string const &text, double &value_out)
{
    char *end = nullptr;
    auto const value = std::strtod(text.c_str(), &end);
    if (!end || end == text.c_str()) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }
    value_out = value;
    return true;
}

Glib::ustring build_firmware_snapshot_text(std::vector<std::string> const &info_lines,
                                          std::vector<std::string> const &modal_lines,
                                          std::vector<std::string> const &offset_lines,
                                          std::vector<std::string> const &setting_lines,
                                          std::vector<std::string> const &errors)
{
    std::ostringstream out;
    auto append_section = [&out](char const *title, std::vector<std::string> const &lines) {
        if (lines.empty()) {
            return;
        }
        out << "[" << title << "]\n";
        for (auto const &line : lines) {
            out << line << "\n";
        }
        out << "\n";
    };
    append_section("I", info_lines);
    append_section("G", modal_lines);
    append_section("#", offset_lines);
    append_section("$", setting_lines);
    if (!errors.empty()) {
        out << "[errors]\n";
        for (auto const &line : errors) {
            out << line << "\n";
        }
    }
    auto const text = out.str();
    return text.empty() ? Glib::ustring(_("尚未读取固件参数。")) : Glib::ustring(text);
}


bool parse_tcp_device_spec(std::string const &spec, std::string &host_out, int &port_out)
{
    std::string s = spec;
    if (s.rfind("tcp://", 0) == 0) {
        s.erase(0, 6);
    }
    auto const colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
        return false;
    }
    host_out = s.substr(0, colon);
    try {
        int const p = std::stoi(s.substr(colon + 1));
        if (p <= 0 || p > 65535) {
            return false;
        }
        port_out = p;
        return !host_out.empty();
    } catch (...) {
        return false;
    }
}

Glib::ustring describe_probe_failure_ui(Glib::ustring const &device, int baud,
                                        Inkscape::Axidraw::GrblProbeResult const &probe)
{
    if (probe.response_line.empty()) {
        return Glib::ustring::compose(
            _("在 %1（%2 波特）上没有收到 GRBL 响应。请检查串口、波特率以及控制器供电。"), device, baud);
    }
    return Glib::ustring::compose(_("串口 %1（%2 波特）已有响应，但看起来不像 GRBL 控制器：\n%3"), device, baud,
                                  Glib::ustring(probe.response_line));
}

} // namespace

namespace Inkscape::UI::Dialog {

GrblControlPanel::GrblControlPanel()
    : DialogBase("/dialogs/grblcontrol", "GrblControl")
    , _btn_mech_home(_("机械归零(_H)（$H）"))
    , _btn_yp(_("_Y+"))
    , _btn_set_origin(_("设为原点(_E)（G92）"))
    , _btn_xm(_("_X-"))
    , _btn_goto_work_zero(_("前往工作 XY 零点(_O)"))
    , _btn_xp(_("_X+"))
    , _btn_reset(_("重置控制器(_R)"))
    , _btn_ym(_("_Y-"))
    , _btn_pen_up(_("抬笔(_P)"))
    , _btn_pen_down(_("落笔(_D)"))
    , _btn_motors(_("电机休眠(_O)（$SLP）"))
    , _btn_clear_alarm(_("清除报警(_M)（$X）"))
    , _btn_fit_to_bed(_("缩放到机器行程内"))
    , _btn_center_to_bed(_("居中到机器行程"))
    , _btn_restore_page_size(_("恢复原页面尺寸"))
    , _btn_load_gcode(_("载入 G-code(_L)..."))
    , _btn_fill_from_drawing(_("从图稿填充(_D)"))
    , _btn_save_gcode(_("G-code 另存为(_A)..."))
    , _btn_send_gcode(_("发送到机器(_S)"))
    , _btn_cancel_gcode(_("取消发送(_C)"))
    , _port_lbl(_("串口"))
    , _btn_refresh_ports(_("刷新端口"))
    , _chk_canvas_plot_preview(_("文档空间预览(_V)"))
    , _chk_machine_space_preview(_("机器空间预览(_P)（mm -> 画布）"))
    , _chk_send_from_cursor_line(_("仅从光标所在行向下发送(_C)"))
    , _chk_sync_page_to_bed(_("连接/同步时把页面改成机器行程（可恢复）"))
    , _btn_read_firmware(_("同步绘图机参数"))
    , _btn_read_radio_mode(_("读取模式"))
    , _btn_apply_radio_mode(_("应用无线模式"))
    , _chk_swap_xy(_("交换 X/Y"))
    , _chk_invert_x(_("反转 X"))
    , _chk_invert_y(_("反转 Y"))
    , _chk_flip_y(_("按页面高度镜像 Y"))
    , _chk_align_origin(_("左下角对齐到机器原点"))
    , _chk_clip_bed(_("限制在机器床面内"))
    , _chk_long_pen_up(_("长距离空走时高抬笔"))
    , _chk_near_connect(_("近距离自动连笔"))
    , _chk_sparse_sampling(_("排线抽稀"))
    , _chk_manual_pen_change_to_home(_("手动换笔时先回到原点"))
    , _chk_manual_pen_change_prompt(_("手动换笔时弹出确认提示"))
    , _chk_tool_change_point(_("换笔前先去换笔点"))
{
    build_ui();
}

GrblControlPanel::~GrblControlPanel()
{
    ensure_machine_status_poll(false);
    std::lock_guard const lk(_port_mutex);
    link_close();
}

void GrblControlPanel::on_map()
{
    DialogBase::on_map();
    refresh_port_list();
    schedule_plot_feedback_refresh(true);
    if (_btn_connect.get_active()) {
        ensure_machine_status_poll(true);
    }
}

void GrblControlPanel::on_unmap()
{
    clear_plot_preview_overlay();
    _plot_feedback_refresh_timer.disconnect();
    _plot_feedback_refresh_preview_requested = false;
    _plot_feedback_refresh_dispatch_pending = false;
    ensure_machine_status_poll(false);
    DialogBase::on_unmap();
}

void GrblControlPanel::desktopReplaced()
{
    schedule_plot_feedback_refresh(true);
}

void GrblControlPanel::documentReplaced()
{
    clear_page_restore_state();
    schedule_plot_feedback_refresh(true);
}

void GrblControlPanel::selectionChanged(Inkscape::Selection * /*selection*/)
{
    schedule_plot_feedback_refresh(true);
}

void GrblControlPanel::selectionModified(Inkscape::Selection * /*selection*/, guint /*flags*/)
{
    schedule_plot_feedback_refresh(true);
}

void GrblControlPanel::update_page_restore_button()
{
    _btn_restore_page_size.set_sensitive(_has_saved_page_restore && !has_active_gcode_stream());
}

void GrblControlPanel::update_mapping_control_sensitivity(bool const allow_interaction)
{
    _chk_swap_xy.set_sensitive(allow_interaction);
    _chk_invert_x.set_sensitive(allow_interaction);
    _chk_invert_y.set_sensitive(allow_interaction);
    _chk_flip_y.set_sensitive(allow_interaction);
    _chk_align_origin.set_sensitive(allow_interaction);
    _chk_clip_bed.set_sensitive(allow_interaction);
    _chk_long_pen_up.set_sensitive(allow_interaction);
    _chk_near_connect.set_sensitive(allow_interaction);
    _chk_sparse_sampling.set_sensitive(allow_interaction);
    _tool_change_mode_combo.set_sensitive(allow_interaction);
    _chk_manual_pen_change_to_home.set_sensitive(allow_interaction && _tool_change_mode_combo.get_active_id() == k_tool_change_mode_manual);
    _chk_manual_pen_change_prompt.set_sensitive(allow_interaction && _tool_change_mode_combo.get_active_id() == k_tool_change_mode_manual);
    _chk_tool_change_point.set_sensitive(allow_interaction && _tool_change_mode_combo.get_active_id() == k_tool_change_mode_m6);
    _bed_width_spin.set_sensitive(allow_interaction && _chk_clip_bed.get_active());
    _bed_depth_spin.set_sensitive(allow_interaction && _chk_clip_bed.get_active());
    _long_pen_up_spin.set_sensitive(allow_interaction && _chk_long_pen_up.get_active());
    _long_move_dist_spin.set_sensitive(allow_interaction && _chk_long_pen_up.get_active());
    _near_connect_dist_spin.set_sensitive(allow_interaction && _chk_near_connect.get_active());
    _sparse_keep_every_spin.set_sensitive(allow_interaction && _chk_sparse_sampling.get_active());
    bool const tool_change_point_sensitive =
        allow_interaction && _tool_change_mode_combo.get_active_id() == k_tool_change_mode_m6 && _chk_tool_change_point.get_active();
    _tool_change_x_spin.set_sensitive(tool_change_point_sensitive);
    _tool_change_y_spin.set_sensitive(tool_change_point_sensitive);
}

GrblControlPanel::RuntimePhase GrblControlPanel::get_runtime_phase() const
{
    if (_gcode_sending.load(std::memory_order_acquire)) {
        if (_gcode_cancel.load(std::memory_order_acquire)) {
            return RuntimePhase::gcode_cancelling;
        }
        return RuntimePhase::gcode_sending;
    }
    if (_connecting.load(std::memory_order_acquire)) {
        return RuntimePhase::connecting;
    }
    if (_firmware_syncing.load(std::memory_order_acquire)) {
        return RuntimePhase::firmware_sync;
    }
    return RuntimePhase::idle;
}

bool GrblControlPanel::has_active_gcode_stream() const
{
    auto const phase = get_runtime_phase();
    return phase == RuntimePhase::gcode_sending || phase == RuntimePhase::gcode_cancelling;
}

bool GrblControlPanel::is_runtime_busy() const
{
    return get_runtime_phase() != RuntimePhase::idle;
}

void GrblControlPanel::refresh_runtime_ui_state()
{
    auto const phase = get_runtime_phase();
    bool const sending = has_active_gcode_stream();
    bool const cancel_requested = phase == RuntimePhase::gcode_cancelling;
    bool const allow_interaction = !is_runtime_busy();

    _btn_connect.set_sensitive(allow_interaction);
    _btn_read_firmware.set_sensitive(allow_interaction);
    _radio_mode_combo.set_sensitive(allow_interaction);
    _radio_pwd.set_sensitive(allow_interaction);
    _chk_radio_restart.set_sensitive(allow_interaction);
    _btn_read_radio_mode.set_sensitive(allow_interaction);
    _btn_apply_radio_mode.set_sensitive(allow_interaction);
    _btn_load_gcode.set_sensitive(allow_interaction);
    _btn_fill_from_drawing.set_sensitive(allow_interaction);
    _btn_send_from_drawing.set_sensitive(allow_interaction);
    _btn_save_gcode.set_sensitive(allow_interaction);
    _chk_send_from_cursor_line.set_sensitive(allow_interaction);
    _chk_sync_page_to_bed.set_sensitive(allow_interaction);
    _chk_canvas_plot_preview.set_sensitive(allow_interaction);
    _chk_machine_space_preview.set_sensitive(allow_interaction);
    update_mapping_control_sensitivity(allow_interaction);
    _start_gcode_view.set_sensitive(allow_interaction);
    _end_gcode_view.set_sensitive(allow_interaction);
    _gcode_view.set_sensitive(allow_interaction);
    _btn_send_gcode.set_sensitive(allow_interaction);
    _btn_cancel_gcode.set_sensitive(sending && !cancel_requested);
    _btn_cancel_gcode.set_label(cancel_requested ? _("停止请求中...") : _("取消发送(_C)"));
    for (auto *b : {&_btn_mech_home, &_btn_yp, &_btn_set_origin, &_btn_xm, &_btn_goto_work_zero, &_btn_xp, &_btn_reset, &_btn_ym,
                    &_btn_pen_up, &_btn_pen_down, &_btn_motors, &_btn_clear_alarm, &_btn_fit_to_bed, &_btn_center_to_bed,
                    &_btn_restore_page_size}) {
        b->set_sensitive(allow_interaction);
    }
    _jog_dist.set_sensitive(allow_interaction);
    update_page_restore_button();
    update_connection_controls();
}

void GrblControlPanel::set_connecting_state(bool const active)
{
    _connecting.store(active, std::memory_order_release);
    refresh_runtime_ui_state();
}

void GrblControlPanel::set_firmware_syncing_state(bool const active)
{
    _firmware_syncing.store(active, std::memory_order_release);
    refresh_runtime_ui_state();
}

bool GrblControlPanel::begin_firmware_sync()
{
    bool const already_syncing = _firmware_syncing.exchange(true, std::memory_order_acq_rel);
    if (already_syncing) {
        return false;
    }
    refresh_runtime_ui_state();
    ensure_machine_status_poll(false);
    return true;
}

SPPage *GrblControlPanel::get_target_page(SPDocument *doc) const
{
    if (!doc) {
        return nullptr;
    }
    auto &page_manager = doc->getPageManager();
    if (!page_manager.hasPages()) {
        return nullptr;
    }
    if (auto *page = page_manager.getSelected()) {
        return page;
    }
    return page_manager.getFirstPage();
}

void GrblControlPanel::request_canvas_redraw() const
{
    if (auto *desk = getDesktop()) {
        if (auto *canvas = desk->getCanvas()) {
            canvas->queue_draw();
        }
    }
}

void GrblControlPanel::capture_page_restore_state(SPDocument *doc)
{
    if (!doc || _has_saved_page_restore) {
        return;
    }
    _saved_doc_width_px = doc->getWidth().value("px");
    _saved_doc_height_px = doc->getHeight().value("px");
    if (auto *page = get_target_page(doc)) {
        auto rect = page->getRect();
        _saved_page_width_px = rect.width();
        _saved_page_height_px = rect.height();
    } else {
        _saved_page_width_px = _saved_doc_width_px;
        _saved_page_height_px = _saved_doc_height_px;
    }
    _has_saved_page_restore = true;
    update_page_restore_button();
}

void GrblControlPanel::clear_page_restore_state()
{
    _has_saved_page_restore = false;
    _saved_doc_width_px = 0.0;
    _saved_doc_height_px = 0.0;
    _saved_page_width_px = 0.0;
    _saved_page_height_px = 0.0;
    update_page_restore_button();
}

void GrblControlPanel::apply_document_and_page_size_px(SPDocument *doc, double const doc_width_px, double const doc_height_px,
                                                       double const page_width_px, double const page_height_px)
{
    if (!doc) {
        return;
    }
    doc->setWidthAndHeight(Inkscape::Util::Quantity(doc_width_px, "px"),
                           Inkscape::Util::Quantity(doc_height_px, "px"), true);
    if (auto *page = get_target_page(doc)) {
        auto rect = page->getRect();
        page->setRect(Geom::Rect::from_xywh(rect.min(), Geom::Point(page_width_px, page_height_px)));
    }
    doc->ensureUpToDate();
    request_canvas_redraw();
}

void GrblControlPanel::post_status(Glib::ustring const &text, bool const is_error)
{
    Glib::signal_idle().connect_once(sigc::track_object([this, text, is_error] {
        _status.set_use_markup(is_error);
        if (is_error) {
            _status.set_markup("<span foreground=\"red\">" + Glib::Markup::escape_text(text) + "</span>");
        } else {
            _status.set_text(text);
        }
    }, *this));
}

void GrblControlPanel::post_machine_status(Glib::ustring const &text)
{
    Glib::signal_idle().connect_once(sigc::track_object([this, text] {
        _machine_status.set_text(text);
    }, *this));
}

void GrblControlPanel::disconnect_controller(bool const announce_status)
{
    set_connecting_state(false);
    ensure_machine_status_poll(false);
    {
        std::lock_guard const lk(_port_mutex);
        if (link_is_open()) {
            link_close();
            if (announce_status) {
                post_status(_("控制器连接已关闭。"), false);
            }
        }
    }
    post_machine_status({});
}

void GrblControlPanel::finish_connect_attempt_ui(bool const keep_connect_active, Glib::ustring const &status,
                                                 bool const is_error, bool const clear_machine_status)
{
    set_connecting_state(false);
    if (!_btn_connect.get_active()) {
        return;
    }
    if (!keep_connect_active) {
        _btn_connect.set_active(false);
    }
    if (clear_machine_status) {
        post_machine_status({});
    }
    post_status(status, is_error);
}

void GrblControlPanel::finalize_successful_connection_ui(Glib::ustring const &device,
                                                         Inkscape::Axidraw::GrblProbeResult const &probe)
{
    set_connecting_state(false);
    if (!_btn_connect.get_active()) {
        disconnect_controller(false);
        return;
    }

    if (probe.response_line.empty()) {
        post_status(Glib::ustring::compose(_("已连接到 %1"), device), false);
    } else {
        post_status(Glib::ustring::compose(_("已连接到 %1\n控制器：%2"), device, Glib::ustring(probe.response_line)), false);
        post_machine_status(Glib::ustring(probe.response_line));
    }

    ensure_machine_status_poll(true);
    on_read_firmware_settings();
    schedule_plot_feedback_refresh(false);
    Glib::signal_timeout().connect_once(sigc::track_object([this] {
        if (_btn_connect.get_active() && get_runtime_phase() != RuntimePhase::connecting) {
            on_read_firmware_settings();
        }
    }, *this), 1200);
}

Glib::ustring GrblControlPanel::get_busy_reason_for_phase(RuntimePhase const phase, BusyReasonContext const context) const
{
    switch (phase) {
        case RuntimePhase::connecting:
            if (context == BusyReasonContext::send_action) {
                return _("当前正在连接绘图机，请等连接完成后再发送。");
            }
            if (context == BusyReasonContext::export_action) {
                return _("当前正在连接绘图机，请等连接完成后再执行。");
            }
            return _("当前正在连接绘图机，请稍候再试。");
        case RuntimePhase::firmware_sync:
            if (context == BusyReasonContext::send_action) {
                return _("当前正在同步固件参数，请等同步完成后再发送。");
            }
            if (context == BusyReasonContext::export_action) {
                return _("当前正在同步固件参数，请等同步完成后再执行。");
            }
            return _("当前正在同步固件参数，请稍候再试。");
        case RuntimePhase::gcode_sending:
        case RuntimePhase::gcode_cancelling:
            return _("当前正在发送任务，请先等待完成或取消发送。");
        case RuntimePhase::idle:
        default:
            return {};
    }
}

bool GrblControlPanel::get_busy_reason(bool const block_connecting, bool const block_firmware_sync,
                                       bool const block_gcode_sending, Glib::ustring &reason) const
{
    auto const phase = get_runtime_phase();
    bool const blocked = (block_connecting && phase == RuntimePhase::connecting) ||
                         (block_firmware_sync && phase == RuntimePhase::firmware_sync) ||
                         (block_gcode_sending && has_active_gcode_stream());
    if (blocked) {
        reason = get_busy_reason_for_phase(phase, BusyReasonContext::generic);
        return true;
    }
    return false;
}

bool GrblControlPanel::is_machine_command_blocked(Glib::ustring &reason) const
{
    return get_busy_reason(true, true, true, reason);
}

bool GrblControlPanel::is_export_operation_blocked(Glib::ustring &reason) const
{
    auto const phase = get_runtime_phase();
    if (phase != RuntimePhase::idle) {
        reason = get_busy_reason_for_phase(phase, BusyReasonContext::export_action);
        return true;
    }
    return false;
}

void GrblControlPanel::load_mapping_preferences_to_ui()
{
    auto *prefs = Inkscape::Preferences::get();
    _suspend_mapping_sync = true;
    _chk_sync_page_to_bed.set_active(prefs->getBool(k_pref_sync_page_to_bed, true));
    bool const auto_pause_between_layers = prefs->getBool(k_pref_auto_pause_between_layers, false);
    bool const manual_pen_change = prefs->getBool(k_pref_manual_pen_change, false);
    bool const tool_change_m6 = prefs->getBool(k_pref_tool_change_m6, false);
    _chk_swap_xy.set_active(prefs->getBool(k_pref_swap_xy, false));
    _chk_invert_x.set_active(prefs->getBool(k_pref_invert_x, false));
    _chk_invert_y.set_active(prefs->getBool(k_pref_invert_y, false));
    _chk_flip_y.set_active(prefs->getBool(k_pref_flip_y, false));
    _chk_align_origin.set_active(prefs->getBool(k_pref_align_origin, false));
    _chk_clip_bed.set_active(prefs->getBool(k_pref_clip_bed, false));
    _chk_long_pen_up.set_active(prefs->getBool(k_pref_long_pen_up, false));
    _chk_near_connect.set_active(prefs->getBool(k_pref_near_connect, false));
    _chk_sparse_sampling.set_active(prefs->getBool(k_pref_sparse_sampling, false));
    if (tool_change_m6) {
        _tool_change_mode_combo.set_active_id(k_tool_change_mode_m6);
    } else if (auto_pause_between_layers && manual_pen_change) {
        _tool_change_mode_combo.set_active_id(k_tool_change_mode_manual);
    } else {
        _tool_change_mode_combo.set_active_id(k_tool_change_mode_none);
    }
    _chk_manual_pen_change_to_home.set_active(prefs->getBool(k_pref_pen_change_to_home, true));
    _chk_manual_pen_change_prompt.set_active(prefs->getBool(k_pref_pen_change_prompt, true));
    _chk_tool_change_point.set_active(prefs->getBool(k_pref_tool_change_point, false));
    _bed_width_spin.set_value(prefs->getDoubleLimited(k_pref_bed_width, 300.0, 1.0, 2000.0));
    _bed_depth_spin.set_value(prefs->getDoubleLimited(k_pref_bed_depth, 200.0, 1.0, 2000.0));
    _long_pen_up_spin.set_value(prefs->getDoubleLimited(k_pref_long_pen_up_mm, 10.0, -1000.0, 1000.0));
    _long_move_dist_spin.set_value(prefs->getDoubleLimited(k_pref_long_move_dist, 20.0, 0.0, 100000.0));
    _near_connect_dist_spin.set_value(prefs->getDoubleLimited(k_pref_near_connect_dist, 0.3, 0.0, 1000.0));
    _sparse_keep_every_spin.set_value(prefs->getIntLimited(k_pref_sparse_keep_every, 1, 1, 64));
    _tool_change_x_spin.set_value(prefs->getDouble(k_pref_tool_change_x));
    _tool_change_y_spin.set_value(prefs->getDouble(k_pref_tool_change_y));
    if (auto const buf = _start_gcode_view.get_buffer()) {
        buf->set_text(prefs->getString(k_pref_start_gcode, ""));
    }
    if (auto const buf = _end_gcode_view.get_buffer()) {
        buf->set_text(prefs->getString(k_pref_end_gcode, ""));
    }
    update_tool_change_mode_ui();
    update_mapping_control_sensitivity(true);
    _suspend_mapping_sync = false;
}

void GrblControlPanel::save_mapping_preferences_from_ui(bool const refresh_preview)
{
    if (_suspend_mapping_sync) {
        return;
    }
    auto *prefs = Inkscape::Preferences::get();
    prefs->setBool(k_pref_swap_xy, _chk_swap_xy.get_active());
    prefs->setBool(k_pref_sync_page_to_bed, _chk_sync_page_to_bed.get_active());
    prefs->setBool(k_pref_invert_x, _chk_invert_x.get_active());
    prefs->setBool(k_pref_invert_y, _chk_invert_y.get_active());
    prefs->setBool(k_pref_flip_y, _chk_flip_y.get_active());
    prefs->setBool(k_pref_align_origin, _chk_align_origin.get_active());
    prefs->setBool(k_pref_clip_bed, _chk_clip_bed.get_active());
    prefs->setBool(k_pref_long_pen_up, _chk_long_pen_up.get_active());
    prefs->setBool(k_pref_near_connect, _chk_near_connect.get_active());
    prefs->setBool(k_pref_sparse_sampling, _chk_sparse_sampling.get_active());
    auto const tool_change_mode = _tool_change_mode_combo.get_active_id();
    bool const manual_pen_change = tool_change_mode == k_tool_change_mode_manual;
    bool const tool_change_m6 = tool_change_mode == k_tool_change_mode_m6;
    prefs->setBool(k_pref_auto_pause_between_layers, manual_pen_change || tool_change_m6);
    prefs->setBool(k_pref_manual_pen_change, manual_pen_change);
    prefs->setBool(k_pref_pen_change_to_home, _chk_manual_pen_change_to_home.get_active());
    prefs->setBool(k_pref_pen_change_prompt, _chk_manual_pen_change_prompt.get_active());
    prefs->setBool(k_pref_tool_change_m6, tool_change_m6);
    prefs->setBool(k_pref_tool_change_point, tool_change_m6 && _chk_tool_change_point.get_active());
    prefs->setDouble(k_pref_bed_width, _bed_width_spin.get_value());
    prefs->setDouble(k_pref_bed_depth, _bed_depth_spin.get_value());
    prefs->setDouble(k_pref_long_pen_up_mm, _long_pen_up_spin.get_value());
    prefs->setDouble(k_pref_long_move_dist, _long_move_dist_spin.get_value());
    prefs->setDouble(k_pref_near_connect_dist, _near_connect_dist_spin.get_value());
    prefs->setInt(k_pref_sparse_keep_every, static_cast<int>(_sparse_keep_every_spin.get_value()));
    prefs->setDouble(k_pref_tool_change_x, _tool_change_x_spin.get_value());
    prefs->setDouble(k_pref_tool_change_y, _tool_change_y_spin.get_value());
    if (auto const buf = _start_gcode_view.get_buffer()) {
        prefs->setString(k_pref_start_gcode, buf->get_text());
    }
    if (auto const buf = _end_gcode_view.get_buffer()) {
        prefs->setString(k_pref_end_gcode, buf->get_text());
    }
    prefs->save();
    update_tool_change_mode_ui();
    bool const allow_interaction = !has_active_gcode_stream();
    update_mapping_control_sensitivity(allow_interaction);
    if (allow_interaction) {
        refresh_plot_feedback(refresh_preview);
    }
}

void GrblControlPanel::update_tool_change_mode_ui()
{
    update_mapping_control_sensitivity(!has_active_gcode_stream());
}

bool GrblControlPanel::is_plot_feedback_blocked() const
{
    return get_runtime_phase() != RuntimePhase::idle;
}

bool GrblControlPanel::require_active_plot_target(SPDocument *&doc, SPDesktop *&desktop, bool const clear_preview_on_failure)
{
    doc = getDocument();
    desktop = getDesktop();
    if (doc && desktop) {
        return true;
    }

    post_status(_("没有活动文档或桌面。"), true);
    if (clear_preview_on_failure) {
        clear_plot_preview_overlay();
    }
    return false;
}

void GrblControlPanel::refresh_plot_feedback_after_gcode_change()
{
    schedule_plot_feedback_refresh(has_plot_preview_enabled());
}

Gtk::Window *GrblControlPanel::get_dialog_parent_window(char const *missing_parent_message)
{
    auto *win = dynamic_cast<Gtk::Window *>(get_root());
    if (!win) {
        post_status(_(missing_parent_message), true);
    }
    return win;
}

bool GrblControlPanel::get_editor_gcode_text(std::string &text, bool const send_from_cursor, guint *editor_line_1)
{
    auto const buf = _gcode_view.get_buffer();
    if (!buf) {
        text.clear();
        return false;
    }

    guint line_1 = 1;
    if (send_from_cursor) {
        Glib::RefPtr<Gtk::TextMark> const ins = buf->get_insert();
        Gtk::TextBuffer::iterator const it_mark = buf->get_iter_at_mark(ins);
        line_1 = static_cast<guint>(it_mark.get_line()) + 1;
        Gtk::TextBuffer::iterator const line0 = buf->get_iter_at_line(it_mark.get_line());
        text = buf->get_text(line0, buf->end(), false).raw();
    } else {
        text = buf->get_text().raw();
    }

    if (editor_line_1) {
        *editor_line_1 = line_1;
    }
    return true;
}

void GrblControlPanel::refresh_plot_summaries()
{
    auto *doc = getDocument();
    auto *desktop = getDesktop();
    if (!doc || !desktop) {
        set_no_active_document_summaries(_job_summary, _layout_scale_summary);
        return;
    }

    GrblPlotStats stats{};
    std::string err;
    if (!analyze_plot_for_feedback(*this, doc, desktop, stats, err)) {
        set_analysis_error_summaries(_job_summary, _layout_scale_summary, err);
        return;
    }

    std::ostringstream summary;
    summary << "<b>任务概览</b>\n";
    summary << _("图层数：") << stats.layer_count << _(" 个");
    summary << "    " << _("笔画数：") << stats.stroke_count << _(" 条");
    if (stats.tool_change_count > 0) {
        summary << "    " << _("换笔：") << stats.tool_change_count << _(" 次");
    }
    summary << "\n" << _("预计时长：") << Glib::Markup::escape_text(format_duration_compact(stats.estimated_duration_sec));
    if (stats.has_bounds_mm) {
        summary << "    " << _("范围：");
        summary << std::fixed << std::setprecision(1) << (stats.max_x_mm - stats.min_x_mm) << " x "
                << (stats.max_y_mm - stats.min_y_mm) << " mm";
    }
    if (stats.has_length_stats && (stats.draw_length_mm + stats.travel_length_mm) > 1e-9) {
        double const total = stats.draw_length_mm + stats.travel_length_mm;
        double const air = (stats.travel_length_mm / total) * 100.0;
        summary << "\n" << _("落笔/空走：");
        summary << std::fixed << std::setprecision(1) << stats.draw_length_mm << " / " << stats.travel_length_mm
                << " mm";
        summary << "    " << _("空走占比：") << std::fixed << std::setprecision(1) << air << "%";
    }

    LayoutScaleMetrics metrics;
    Glib::ustring layout_error;
    if (stats.has_bounds_mm &&
        get_layout_scale_metrics_from_bounds_mm(stats.max_x_mm - stats.min_x_mm, stats.max_y_mm - stats.min_y_mm,
                                                _bed_width_spin.get_value(), _bed_depth_spin.get_value(), metrics,
                                                layout_error)) {
        summary << "\n" << _("适配缩放：");
        if (metrics.fits_without_scaling) {
            summary << _("无需缩小");
        } else {
            summary << std::fixed << std::setprecision(1) << (metrics.fit_scale * 100.0) << "%";
        }
        summary << "    " << _("床面占用：X ");
        summary << std::fixed << std::setprecision(1) << metrics.fill_x_pct << "%";
        summary << " / Y " << std::fixed << std::setprecision(1) << metrics.fill_y_pct << "%";
    }

    _job_summary.set_markup(summary.str());
    set_layout_scale_summary_from_stats(_layout_scale_summary, stats, _bed_width_spin.get_value(),
                                        _bed_depth_spin.get_value());
}

bool GrblControlPanel::has_plot_preview_enabled() const
{
    return _chk_canvas_plot_preview.get_active() || _chk_machine_space_preview.get_active();
}

void GrblControlPanel::schedule_plot_feedback_refresh(bool const refresh_preview)
{
    _plot_feedback_refresh_preview_requested = _plot_feedback_refresh_preview_requested || refresh_preview;
    if (is_plot_feedback_blocked()) {
        return;
    }
    if (refresh_preview) {
        clear_plot_preview_overlay();
    }
    if (_plot_feedback_refresh_dispatch_pending || _plot_feedback_refresh_timer.connected()) {
        return;
    }
    _plot_feedback_refresh_dispatch_pending = true;
    Glib::signal_idle().connect_once(sigc::track_object([this, refresh_preview] {
        _plot_feedback_refresh_dispatch_pending = false;
        refresh_plot_feedback(refresh_preview);
    }, *this));
}

void GrblControlPanel::refresh_plot_feedback(bool const refresh_preview)
{
    _plot_feedback_refresh_preview_requested = _plot_feedback_refresh_preview_requested || refresh_preview;
    if (is_plot_feedback_blocked()) {
        return;
    }
    if (_plot_feedback_refresh_timer.connected()) {
        return;
    }
    _plot_feedback_refresh_timer = Glib::signal_timeout().connect(sigc::track_object([this] {
        bool const refresh_preview_now = _plot_feedback_refresh_preview_requested;
        _plot_feedback_refresh_preview_requested = false;
        _plot_feedback_refresh_timer.disconnect();
        refresh_plot_summaries();
        if (refresh_preview_now && has_plot_preview_enabled()) {
            sync_plot_preview_overlay();
        }
        return false;
    }, *this), 120);
}

bool GrblControlPanel::begin_gcode_stream_ui(Glib::ustring const &status)
{
    auto const phase = get_runtime_phase();
    if (phase == RuntimePhase::gcode_sending || phase == RuntimePhase::gcode_cancelling) {
        return false;
    }
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        blocked_reason = get_busy_reason_for_phase(phase, BusyReasonContext::send_action);
        post_status(blocked_reason, true);
        return false;
    }
    set_gcode_stream_ui_active(true);
    if (!status.empty()) {
        post_status(status, false);
    }
    return true;
}

void GrblControlPanel::post_gcode_stream_result(std::string const &err)
{
    if (err == grbl_error_user_cancelled()) {
        post_status(_("发送已停止（已取消）。"), false);
    } else {
        post_status(Glib::ustring(Inkscape::Axidraw::grbl_error_to_user_message(err)), true);
    }
}

bool GrblControlPanel::link_is_open() const
{
    return (_port && _port->is_open()) || (_tcp_port && _tcp_port->is_open());
}

bool GrblControlPanel::link_write_bytes(void const *data, size_t len)
{
    if (_port && _port->is_open()) {
        return _port->write_bytes(data, len);
    }
    if (_tcp_port && _tcp_port->is_open()) {
        return _tcp_port->write_bytes(data, len);
    }
    return false;
}

bool GrblControlPanel::link_read_line(std::string &out, int timeout_ms)
{
    if (_port && _port->is_open()) {
        return _port->read_line(out, timeout_ms);
    }
    if (_tcp_port && _tcp_port->is_open()) {
        return _tcp_port->read_line(out, timeout_ms);
    }
    return false;
}

bool GrblControlPanel::link_write_line(std::string const &line, std::string &err_out)
{
    if (_port && _port->is_open()) {
        return grbl_send_line(*_port, line, err_out);
    }
    if (_tcp_port && _tcp_port->is_open()) {
        return grbl_send_line(*_tcp_port, line, err_out);
    }
    err_out = "not connected";
    return false;
}

void GrblControlPanel::link_purge_io()
{
    if (_port && _port->is_open()) {
        _port->purge_io();
    } else if (_tcp_port && _tcp_port->is_open()) {
        _tcp_port->purge_io();
    }
}

void GrblControlPanel::link_close()
{
    if (_port) {
        _port->close();
        _port.reset();
    }
    if (_tcp_port) {
        _tcp_port->close();
        _tcp_port.reset();
    }
}

void GrblControlPanel::ensure_machine_status_poll(bool const on)
{
    _machine_status_poll.disconnect();
    if (!on) {
        return;
    }
    _machine_status_poll = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &GrblControlPanel::on_machine_status_poll_timeout), 1500);
}

bool GrblControlPanel::on_machine_status_poll_timeout()
{
    if (!_btn_connect.get_active()) {
        return false;
    }
    auto const phase = get_runtime_phase();
    if (phase == RuntimePhase::gcode_sending || phase == RuntimePhase::gcode_cancelling ||
        phase == RuntimePhase::firmware_sync) {
        return true;
    }

    std::thread([this] {
        std::unique_lock<std::mutex> lk(_port_mutex, std::try_to_lock);
        if (!lk.owns_lock()) {
            return;
        }
        if (!link_is_open()) {
            return;
        }
        char const q = '?';
        if (!link_write_bytes(&q, 1)) {
            return;
        }
        std::string line;
        if (!link_read_line(line, 400)) {
            return;
        }
        // If a prior command left an "ok" ahead of the report, read once more.
        if (line == "ok" && link_read_line(line, 200)) {
            // use second line
        }
        post_machine_status(Glib::ustring(line));
    }).detach();

    return true;
}

void GrblControlPanel::refresh_port_list()
{
    _suspend_port_combo = true;
    auto *prefs = Inkscape::Preferences::get();
    Glib::ustring const cur_pref = prefs->getString(k_pref_device);
    Glib::ustring const net_host = prefs->getString(k_pref_net_host);
    int const net_port = prefs->getIntLimited(k_pref_net_port, 23, 1, 65535);
    std::vector<std::string> ports = Inkscape::Axidraw::enumerate_serial_ports();
    _port_combo.remove_all();

    bool seen_pref = false;
    for (auto const &p : ports) {
        Glib::ustring const u(p);
        _port_combo.append(u, u);
        if (u == cur_pref) {
            seen_pref = true;
        }
    }
    if (!cur_pref.empty() && !seen_pref) {
        _port_combo.append(cur_pref, cur_pref);
    }
    if (!net_host.empty()) {
        Glib::ustring const tcp_spec = "tcp://" + net_host + ":" + std::to_string(net_port);
        if (tcp_spec != cur_pref) {
            _port_combo.append(tcp_spec, tcp_spec);
        }
    }

    if (!cur_pref.empty()) {
        _port_combo.set_active_id(cur_pref);
    } else if (ports.empty()) {
        // leave empty
    } else {
        _port_combo.set_active(0);
    }
    _suspend_port_combo = false;
}

void GrblControlPanel::on_port_combo_changed()
{
    if (_suspend_port_combo) {
        return;
    }
    Glib::ustring id = _port_combo.get_active_id();
    if (id.empty()) {
        id = _port_combo.get_active_text();
    }
    if (id.empty()) {
        return;
    }
    auto *prefs = Inkscape::Preferences::get();
    prefs->setString(k_pref_device, id);
    prefs->save();
}

void GrblControlPanel::run_action(std::function<void(std::string &)> work, bool const report_ok)
{
    std::thread([this, w = std::move(work), report_ok]() mutable {
        Glib::ustring blocked_reason;
        if (is_machine_command_blocked(blocked_reason)) {
            post_status(blocked_reason, true);
            return;
        }
        std::lock_guard const guard(_port_mutex);
        if (is_machine_command_blocked(blocked_reason)) {
            post_status(blocked_reason, true);
            return;
        }
        if (!link_is_open()) {
            post_status(_("尚未连接。"), true);
            return;
        }
        std::string err;
        w(err);
        if (err.empty()) {
            if (report_ok) {
                post_status(_("操作完成。"), false);
            }
        } else {
            post_status(Glib::ustring(Inkscape::Axidraw::grbl_error_to_user_message(err)), true);
        }
    }).detach();
}

void GrblControlPanel::on_read_firmware_settings()
{
    if (!begin_firmware_sync()) {
        return;
    }
    std::thread([this] {
        scope_exit const finish_sync{[this] {
            Glib::signal_idle().connect_once(sigc::track_object([this] {
                set_firmware_syncing_state(false);
                if (_btn_connect.get_active()) {
                    ensure_machine_status_poll(true);
                }
                schedule_plot_feedback_refresh(false);
            }, *this));
        }};
        auto query_lines_locked = [this](std::string const &command, std::vector<std::string> &lines_out,
                                         std::string &err_out) -> bool {
            lines_out.clear();
            if (!link_is_open()) {
                err_out = "not connected";
                return false;
            }

            link_purge_io();

            bool const sent = (_port && _port->is_open()) ? _port->write_line(command)
                                                          : (_tcp_port && _tcp_port->is_open()
                                                                 ? _tcp_port->write_line(command)
                                                                 : false);
            if (!sent) {
                err_out = "serial write failed";
                return false;
            }

            for (;;) {
                std::string line;
                if (!link_read_line(line, 1800)) {
                    err_out = "timeout waiting for controller response";
                    return false;
                }
                trim_in_place(line);
                if (line.empty()) {
                    continue;
                }
                if (line == command) {
                    continue;
                }
                if (line == "ok") {
                    return true;
                }
                if (Inkscape::Axidraw::grbl_is_error_line(line)) {
                    err_out = line;
                    return false;
                }
                lines_out.push_back(std::move(line));
            }
        };

        std::vector<std::string> info_lines;
        std::vector<std::string> modal_lines;
        std::vector<std::string> offset_lines;
        std::vector<std::string> setting_lines;
        std::vector<std::string> errors;
        GrblFirmwareSnapshot snapshot;

        {
            std::lock_guard const guard(_port_mutex);
            if (!link_is_open()) {
                post_status(_("尚未连接。"), true);
                return;
            }

            auto run_query = [&](std::string const &command, std::vector<std::string> &dest) {
                std::string err;
                if (!query_lines_locked(command, dest, err)) {
                    errors.push_back(command + ": " + Inkscape::Axidraw::grbl_error_to_user_message(err));
                }
            };

            run_query("$I", info_lines);
            run_query("$G", modal_lines);
            run_query("$#", offset_lines);
            run_query("$$", setting_lines);
        }

        for (auto const &line : setting_lines) {
            int code = 0;
            std::string value;
            if (!parse_grbl_setting_line(line, code, value)) {
                continue;
            }
            if (code == 3) {
                try {
                    snapshot.direction_mask = std::stoi(value);
                    snapshot.has_direction_mask = true;
                } catch (...) {
                }
            } else if (code == 130) {
                snapshot.has_x_travel = parse_double_c(value, snapshot.x_travel_mm);
            } else if (code == 131) {
                snapshot.has_y_travel = parse_double_c(value, snapshot.y_travel_mm);
            }
        }

        snapshot.display_text = build_firmware_snapshot_text(info_lines, modal_lines, offset_lines, setting_lines, errors);

        Glib::signal_idle().connect_once(sigc::track_object([this, snapshot] {
            auto apply_snapshot_to_ui = [this](GrblFirmwareSnapshot const &snapshot_in, bool &page_synced_out,
                                               bool &unit_synced_out) {
                bool changed_local = false;
                _suspend_mapping_sync = true;
                if (snapshot_in.has_direction_mask) {
                    _chk_invert_x.set_active((snapshot_in.direction_mask & 0x1) != 0);
                    _chk_invert_y.set_active((snapshot_in.direction_mask & 0x2) != 0);
                    changed_local = true;
                }
                if (snapshot_in.has_x_travel) {
                    _bed_width_spin.set_value(snapshot_in.x_travel_mm);
                    changed_local = true;
                }
                if (snapshot_in.has_y_travel) {
                    _bed_depth_spin.set_value(snapshot_in.y_travel_mm);
                    changed_local = true;
                }
                _suspend_mapping_sync = false;

                page_synced_out = false;
                unit_synced_out = false;
                if (_chk_sync_page_to_bed.get_active() && snapshot_in.has_x_travel && snapshot_in.has_y_travel) {
                    if (auto *doc = getDocument()) {
                        capture_page_restore_state(doc);
                        Inkscape::Util::Quantity const width(snapshot_in.x_travel_mm, "mm");
                        Inkscape::Util::Quantity const height(snapshot_in.y_travel_mm, "mm");
                        apply_document_and_page_size_px(doc, width.value("px"), height.value("px"), width.value("px"),
                                                        height.value("px"));
                        if (auto *nv = doc->getNamedView()) {
                            if (auto *repr = nv->getRepr()) {
                                repr->setAttribute("inkscape:document-units", "mm");
                                unit_synced_out = true;
                            }
                        }
                        if (auto action = doc->getActionGroup()->lookup_action("set-display-unit")) {
                            action->activate(Glib::Variant<Glib::ustring>::create("mm"));
                        }
                        doc->setModifiedSinceSave();
                        Inkscape::DocumentUndo::done(doc, RC_("Undo", "按机器行程同步页面尺寸和单位"), "");
                        page_synced_out = true;
                    }
                }

                return changed_local;
            };
            auto build_sync_status = [](GrblFirmwareSnapshot const &snapshot_in, bool page_synced_in, bool unit_synced_in) {
                std::vector<Glib::ustring> notes;
                if (snapshot_in.has_direction_mask) {
                    notes.emplace_back(Glib::ustring::compose(_("已同步方向反转掩码 $3=%1"), snapshot_in.direction_mask));
                }
                if (snapshot_in.has_x_travel || snapshot_in.has_y_travel) {
                    if (snapshot_in.has_x_travel && snapshot_in.has_y_travel) {
                        notes.emplace_back(Glib::ustring::compose(_("已同步床面尺寸 X=%1 mm, Y=%2 mm"),
                                                                  snapshot_in.x_travel_mm, snapshot_in.y_travel_mm));
                    } else if (snapshot_in.has_x_travel) {
                        notes.emplace_back(Glib::ustring::compose(_("已同步床面宽度 X=%1 mm"), snapshot_in.x_travel_mm));
                    } else {
                        notes.emplace_back(Glib::ustring::compose(_("已同步床面深度 Y=%1 mm"), snapshot_in.y_travel_mm));
                    }
                } else {
                    notes.emplace_back(_("未从固件读取到 $130 / $131 行程参数，因此没有同步页面尺寸。"));
                }
                if (page_synced_in) {
                    notes.emplace_back(
                        Glib::ustring::compose(_("已将当前页面尺寸同步为 %1 x %2 mm"), snapshot_in.x_travel_mm, snapshot_in.y_travel_mm));
                }
                if (unit_synced_in) {
                    notes.emplace_back(_("已将文档单位同步为 mm"));
                }
                if (notes.empty()) {
                    return Glib::ustring(_("已读取固件参数。"));
                }

                std::ostringstream msg;
                msg << _("已读取固件参数。");
                for (auto const &note : notes) {
                    msg << "\n" << note.raw();
                }
                return Glib::ustring(msg.str());
            };
            if (auto const buf = _firmware_info_view.get_buffer()) {
                buf->set_text(snapshot.display_text);
            }

            bool page_synced = false;
            bool unit_synced = false;
            bool const changed = apply_snapshot_to_ui(snapshot, page_synced, unit_synced);

            if (changed) {
                save_mapping_preferences_from_ui(true);
            } else {
                schedule_plot_feedback_refresh(true);
            }
            post_status(build_sync_status(snapshot, page_synced, unit_synced), false);
        }, *this));
    }).detach();
}

void GrblControlPanel::connect_toggle()
{
    bool const want = _btn_connect.get_active();
    if (!want) {
        disconnect_controller(true);
        return;
    }

    auto *prefs = Inkscape::Preferences::get();
    Glib::ustring device = prefs->getString(k_pref_device);
    if (device.empty()) {
        device = _port_combo.get_active_id();
        if (device.empty()) {
            device = _port_combo.get_active_text();
        }
        if (!device.empty()) {
            prefs->setString(k_pref_device, device);
            prefs->save();
        }
    }
    if (device.empty()) {
        _btn_connect.set_active(false);
        post_status(_("请先在上方选择串口，或在“首选项”的 GRBL 标签页中设置“串口设备”。"), true);
        return;
    }

    int const baud = prefs->getIntLimited(k_pref_baud, 115200, 9600, 230400);
    Glib::ustring device_for_thread = device;
    std::string tcp_host;
    int tcp_port = 0;
    bool const tcp_mode = parse_tcp_device_spec(device.raw(), tcp_host, tcp_port);
    if (!tcp_mode && device_for_thread.empty()) {
        Glib::ustring const host = prefs->getString(k_pref_net_host);
        int const net_port = prefs->getIntLimited(k_pref_net_port, 23, 1, 65535);
        if (!host.empty()) {
            device_for_thread = "tcp://" + host + ":" + std::to_string(net_port);
            tcp_host = host.raw();
            tcp_port = net_port;
        }
    }
    bool const use_tcp = !tcp_host.empty() && tcp_port > 0;
    set_connecting_state(true);
    if (use_tcp) {
        post_status(Glib::ustring::compose(_("正在通过 TCP 探测 %1..."), device_for_thread), false);
    } else {
        post_status(Glib::ustring::compose(_("正在以 %2 波特探测 %1..."), device_for_thread, baud), false);
    }

    // Run open/probe on a worker to avoid blocking UI if driver stalls.
    std::thread([this, device_for_thread, baud, use_tcp, tcp_host, tcp_port] {
        auto port = std::make_unique<SerialPort>();
        auto tcp = std::make_unique<Inkscape::Axidraw::TcpPort>();
        bool const opened = use_tcp ? tcp->open(tcp_host, tcp_port) : port->open(device_for_thread.raw(), baud);
        if (!opened) {
            Glib::signal_idle().connect_once(sigc::track_object([this] {
                finish_connect_attempt_ui(false, _("无法打开所选连接。"), true);
            }, *this));
            return;
        }

        auto const probe = use_tcp ? Inkscape::Axidraw::probe_open_grbl(*tcp) : Inkscape::Axidraw::probe_open_grbl(*port);
        if (!probe.ok) {
            Glib::signal_idle().connect_once(sigc::track_object([this, probe, baud, dev = Glib::ustring(device_for_thread)] {
                Glib::ustring status;
                if (dev.rfind("tcp://", 0) == 0) {
                    Glib::ustring const detail = probe.response_line.empty()
                                                     ? _("TCP 上没有收到 GRBL 响应。")
                                                     : Glib::ustring::compose(
                                                           _("TCP 端点已有响应，但看起来不像 GRBL：\n%1"),
                                                           Glib::ustring(probe.response_line));
                    status = Glib::ustring::compose(_("无法连接到 %1。\n%2"), dev, detail);
                } else {
                    status = describe_probe_failure_ui(dev, baud, probe);
                }
                finish_connect_attempt_ui(false, status, true, true);
            }, *this));
            return;
        }

        std::lock_guard guard(_port_mutex);
        link_close();
        if (use_tcp) {
            _tcp_port = std::move(tcp);
        } else {
            _port = std::move(port);
        }
        Glib::signal_idle().connect_once(sigc::track_object([this, dev = Glib::ustring(device_for_thread), probe] {
            finalize_successful_connection_ui(dev, probe);
        }, *this));
    }).detach();
}

double GrblControlPanel::jog_distance_mm() const
{
    auto const t = _jog_dist.get_active_text();
    return std::strtod(t.c_str(), nullptr);
}

double GrblControlPanel::travel_feed_mm_min() const
{
    auto *prefs = Inkscape::Preferences::get();
    return prefs->getDoubleLimited(k_pref_travel, 6000.0, 60.0, 20000.0);
}

void GrblControlPanel::jog_axis(char const axis, double const sign, double const dist_mm, double const feed)
{
    run_action(
        [this, axis, sign, dist_mm, feed](std::string &e) {
            if (dist_mm <= 0) {
                e = _("点动距离必须为正数");
                return;
            }
            double const d0 = dist_mm * sign;
            std::ostringstream dstr;
            dstr.setf(std::ios::fixed);
            dstr << std::setprecision(6) << d0;
            int const ifeed = static_cast<int>(feed + 0.5);
            if (!link_write_line("G21", e)) {
                return;
            }
            if (!link_write_line("G91", e)) {
                return;
            }
            {
                std::ostringstream m;
                m << "G1 " << axis << dstr.str() << " F" << ifeed;
                if (!link_write_line(m.str(), e)) {
                    return;
                }
            }
            if (!link_write_line("G90", e)) {
                return;
            }
        },
        false);
}

void GrblControlPanel::jog_x(double const sign)
{
    double const d = jog_distance_mm();
    double const f = travel_feed_mm_min();
    jog_axis('X', sign, d, f);
}

void GrblControlPanel::jog_y(double const sign)
{
    double const d = jog_distance_mm();
    double const f = travel_feed_mm_min();
    jog_axis('Y', sign, d, f);
}

void GrblControlPanel::soft_reset()
{
    run_action(
        [this](std::string &e) {
            const char c = 0x18;
            if (!link_write_bytes(&c, 1)) {
                e = _("无法向串口写入软复位字节");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            link_purge_io();
            post_status(
                _("软复位已发送。如果端口不再响应，请先断开再重新连接。"), false);
        },
        false);
}

void GrblControlPanel::send_pen_state(bool const up)
{
    run_action([this, up](std::string &e) {
        (void)this;
        auto *prefs = Inkscape::Preferences::get();
        Glib::ustring const pctl = prefs->getString(k_pref_pen_control, "z");
        if (pctl == "m3m5" || pctl == "M3M5") {
            if (!link_write_line(up ? "M5" : "M3 S1000", e)) {
                return;
            }
        } else {
            Glib::ustring const cmd = up ? prefs->getString(k_pref_pen_up, "G1 Z0 F3000") : prefs->getString(k_pref_pen_down, "G1 Z5 F3000");
            if (cmd.empty()) {
                e = up ? _("抬笔命令（首选项中设置）为空") : _("落笔命令（首选项中设置）为空");
                return;
            }
            if (!link_write_line(cmd.raw(), e)) {
                return;
            }
        }
    });
}

void GrblControlPanel::on_fit_document_to_bed()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }

    auto *doc = getDocument();
    Geom::Rect bounds;
    double bed_w_doc = 0.0;
    double bed_h_doc = 0.0;
    Glib::ustring error;
    if (!get_document_bounds_and_bed(doc, _bed_width_spin.get_value(), _bed_depth_spin.get_value(), bounds, bed_w_doc,
                                     bed_h_doc, error, _("当前文档没有可缩放的绘图内容。"))) {
        post_status(error, true);
        return;
    }

    double const content_w = bounds.width();
    double const content_h = bounds.height();
    double scale = std::min(bed_w_doc / content_w, bed_h_doc / content_h);
    if (!(scale > 0.0)) {
        post_status(_("无法计算缩放比例。"), true);
        return;
    }
    if (scale > 1.0) {
        scale = 1.0;
    }

    if (scale < 0.999999) {
        doc->getRoot()->scaleChildItemsRec(Geom::Scale(scale), bounds.min(), false);
    }
    doc->getRoot()->translateChildItems(Geom::Translate(-bounds.min()[Geom::X], -bounds.min()[Geom::Y]));
    doc->setModifiedSinceSave();
    Inkscape::DocumentUndo::done(doc, RC_("Undo", "按机器行程缩放图稿"), "");

    schedule_plot_feedback_refresh(true);

    std::ostringstream msg;
    if (scale < 0.999999) {
        msg << _("已将图稿等比缩小并移动到机器行程内。缩放比例 ");
        msg << std::fixed << std::setprecision(1) << (scale * 100.0) << "%。";
    } else {
        msg << _("图稿本身已小于机器行程，已仅将其移动到机器原点范围内。");
    }
    post_status(Glib::ustring(msg.str()), false);
}

void GrblControlPanel::on_center_document_to_bed()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }

    auto *doc = getDocument();
    Geom::Rect bounds;
    double bed_w_doc = 0.0;
    double bed_h_doc = 0.0;
    Glib::ustring error;
    if (!get_document_bounds_and_bed(doc, _bed_width_spin.get_value(), _bed_depth_spin.get_value(), bounds, bed_w_doc,
                                     bed_h_doc, error, _("当前文档没有可居中的绘图内容。"))) {
        post_status(error, true);
        return;
    }

    double const content_w = bounds.width();
    double const content_h = bounds.height();
    double const dx = ((bed_w_doc - content_w) * 0.5) - bounds.min()[Geom::X];
    double const dy = ((bed_h_doc - content_h) * 0.5) - bounds.min()[Geom::Y];
    doc->getRoot()->translateChildItems(Geom::Translate(dx, dy));
    doc->setModifiedSinceSave();
    Inkscape::DocumentUndo::done(doc, RC_("Undo", "按机器行程居中图稿"), "");

    schedule_plot_feedback_refresh(true);

    post_status(_("已将图稿整体居中到机器行程范围内。"), false);
}

void GrblControlPanel::on_restore_page_size()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    if (!_has_saved_page_restore) {
        post_status(_("当前没有可恢复的原页面尺寸记录。"), true);
        return;
    }

    auto *doc = getDocument();
    if (!doc) {
        post_status(_("没有活动文档。"), true);
        return;
    }

    apply_document_and_page_size_px(doc, _saved_doc_width_px, _saved_doc_height_px, _saved_page_width_px,
                                    _saved_page_height_px);
    doc->setModifiedSinceSave();
    Inkscape::DocumentUndo::done(doc, RC_("Undo", "恢复原页面尺寸"), "");

    clear_page_restore_state();
    schedule_plot_feedback_refresh(true);
    post_status(_("已恢复同步前的页面尺寸。"), false);
}

void GrblControlPanel::update_connection_controls()
{
    auto const phase = get_runtime_phase();
    bool const connecting = phase == RuntimePhase::connecting;
    bool const serial_controls = !is_runtime_busy();

    _port_combo.set_sensitive(serial_controls);
    _btn_refresh_ports.set_sensitive(serial_controls);

    if (connecting) {
        _btn_connect.set_label(_("连接中..."));
    } else if (_btn_connect.get_active()) {
        _btn_connect.set_label(_("断开连接"));
    } else {
        _btn_connect.set_label(_("连接"));
    }
}

void GrblControlPanel::set_controls_sensitive_for_gcode_stream(bool const allow_interaction)
{
    (void)allow_interaction;
    refresh_runtime_ui_state();
}

void GrblControlPanel::set_gcode_stream_ui_active(bool const active)
{
    _gcode_sending.store(active, std::memory_order_release);
    _gcode_cancel.store(false, std::memory_order_release);
    refresh_runtime_ui_state();
}

bool GrblControlPanel::set_gcode_cancel_requested(bool const active)
{
    bool const previous = _gcode_cancel.exchange(active, std::memory_order_acq_rel);
    if (previous == active) {
        return false;
    }
    refresh_runtime_ui_state();
    return true;
}

void GrblControlPanel::finish_gcode_stream_ui()
{
    Glib::signal_idle().connect_once(sigc::track_object([this] {
        set_gcode_stream_ui_active(false);
        schedule_plot_feedback_refresh(false);
    }, *this));
}

void GrblControlPanel::on_cancel_gcode_stream()
{
    if (!has_active_gcode_stream()) {
        return;
    }
    if (!set_gcode_cancel_requested(true)) {
        return;
    }
    post_status(_("正在请求停止发送..."), false);
}

void GrblControlPanel::clear_plot_preview_overlay()
{
    _plot_preview_overlay.reset();
    _plot_preview_machine_overlay.reset();
    _plot_preview_machine_axis_overlay.reset();
    _plot_preview_axis_origin_label.reset();
    _plot_preview_axis_x_label.reset();
    _plot_preview_axis_y_label.reset();
    request_canvas_redraw();
}

void GrblControlPanel::sync_plot_preview_overlay()
{
    clear_plot_preview_overlay();
    if (!_chk_canvas_plot_preview.get_active() && !_chk_machine_space_preview.get_active()) {
        return;
    }
    auto *desk = getDesktop();
    auto *doc = getDocument();
    if (!desk || !doc) {
        return;
    }
    auto params = make_export_params_from_preferences();
    auto ctx = make_export_context(*this, desk);

    constexpr std::size_t k_preview_max_strokes = 12000;
    Geom::Affine const aff = desk->doc2dt();
    Glib::ustring status_note;

    if (_chk_canvas_plot_preview.get_active()) {
        Geom::PathVector pv_doc;
        std::string err;
        std::size_t included = 0;
        std::size_t total = 0;
        if (!build_grbl_plot_preview_pathvector(doc, params, ctx, pv_doc, err, k_preview_max_strokes, &included,
                                                &total)) {
            post_status(make_preview_build_error(err, false), true);
            if (!_chk_machine_space_preview.get_active()) {
                return;
            }
        } else if (!pv_doc.empty()) {
            _plot_preview_overlay = make_canvasitem<CanvasItemBpath>(desk->getCanvasTemp(),
                                                                     transform_pathvector_to_desktop(pv_doc, aff), true);
            configure_preview_overlay(*_plot_preview_overlay, 0x22aaffcc, 1.0);
            status_note = build_preview_status_note(false, included, total, false);
        }
    }

    if (!_chk_machine_space_preview.get_active()) {
        return;
    }

    Geom::PathVector pv_m;
    std::string err_m;
    bool clip_approx = false;
    std::size_t inc_m = 0;
    std::size_t tot_m = 0;
    if (!build_grbl_plot_machine_preview_pathvector_in_doc_space(doc, params, ctx, pv_m, err_m, &clip_approx,
                                                                 k_preview_max_strokes, &inc_m, &tot_m)) {
        post_status(make_preview_build_error(err_m, true), true);
        return;
    }
    if (!pv_m.empty()) {
        _plot_preview_machine_overlay = make_canvasitem<CanvasItemBpath>(
            desk->getCanvasTemp(), transform_pathvector_to_desktop(pv_m, aff), true);
        configure_preview_overlay(*_plot_preview_machine_overlay, 0xff8844cc, 1.25);
        status_note = build_preview_status_note(true, inc_m, tot_m, clip_approx);
    }

    double const bed_w = std::max(1.0, _bed_width_spin.get_value());
    double const bed_h = std::max(1.0, _bed_depth_spin.get_value());
    Geom::Point const origin_dt = Geom::Point(0.0, 0.0) * aff;
    double const axis_len_doc = std::max(18.0, std::min(bed_w, bed_h) * 0.14);

    auto const x_dir_doc = machine_axis_direction(params.swap_xy, params.invert_x, params.invert_y, true);
    auto const y_dir_doc = machine_axis_direction(params.swap_xy, params.invert_x, params.invert_y, false);
    Geom::Point const x_end_dt = (Geom::Point(0.0, 0.0) + x_dir_doc * axis_len_doc) * aff;
    Geom::Point const y_end_dt = (Geom::Point(0.0, 0.0) + y_dir_doc * axis_len_doc) * aff;
    Geom::PathVector axis_pv;
    append_axis_arrow(axis_pv, origin_dt, x_end_dt, 12.0, 5.0);
    append_axis_arrow(axis_pv, origin_dt, y_end_dt, 12.0, 5.0);
    _plot_preview_machine_axis_overlay = make_canvasitem<CanvasItemBpath>(desk->getCanvasTemp(), axis_pv, true);
    configure_preview_overlay(*_plot_preview_machine_axis_overlay, 0x00aa55ee, 2.0);

    _plot_preview_axis_origin_label = make_preview_axis_label(desk, origin_dt + Geom::Point(8.0, -8.0), _("机器原点"),
                                                              0x003344dd);
    _plot_preview_axis_x_label = make_preview_axis_label(desk, x_end_dt + Geom::Point(8.0, -8.0), _("机器 X+"),
                                                         0x005522dd);
    _plot_preview_axis_y_label = make_preview_axis_label(desk, y_end_dt + Geom::Point(8.0, -8.0), _("机器 Y+"),
                                                         0x225500dd);
    if (!status_note.empty()) {
        post_status(status_note, false);
    }
    request_canvas_redraw();
}

void GrblControlPanel::on_fill_gcode_from_document()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    SPDocument *doc = nullptr;
    SPDesktop *desktop = nullptr;
    if (!require_active_plot_target(doc, desktop)) {
        return;
    }

    auto params = make_export_params_from_preferences();
    auto ctx = make_export_context(*this, desktop);

    std::string out;
    std::string err;
    std::size_t strokes = 0;
    GrblPlotStats stats{};
    if (!build_grbl_plot_gcode_string(doc, params, ctx, out, err, &strokes, k_max_gcode_editor_bytes, &stats)) {
        clear_plot_preview_overlay();
        post_status(err.empty() ? Glib::ustring(_("无法根据当前图稿生成 G-code。")) : Glib::ustring(err), true);
        return;
    }
    if (auto const buf = _gcode_view.get_buffer()) {
        buf->set_text(out);
    }
    refresh_plot_feedback_after_gcode_change();
    post_status(make_fill_gcode_status(strokes, stats), false);
}

void GrblControlPanel::on_send_document_direct()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    SPDocument *doc = nullptr;
    SPDesktop *desktop = nullptr;
    if (!require_active_plot_target(doc, desktop)) {
        return;
    }

    auto params = make_export_params_from_preferences();

    if (!_port || !_port->is_open()) {
        if (_tcp_port && _tcp_port->is_open()) {
            post_status(_("“从图稿直接发送”目前仅支持串口直连的流式发送。网络连接请先“从图稿填充”，再发送编辑器中的 G-code。"),
                        true);
        } else {
            post_status(_("尚未连接串口绘图机。"), true);
        }
        return;
    }

    auto base_ctx = make_export_context(*this, desktop);
    bool const use_current_layer_without_selection = base_ctx.use_current_layer_without_selection;
    auto *selection = base_ctx.selection;
    refresh_plot_feedback_after_gcode_change();

    if (!begin_gcode_stream_ui(_("正在按当前图稿直接流式发送到绘图机..."))) {
        return;
    }

    auto *win = dynamic_cast<Gtk::Window *>(get_root());
    if (params.manual_pen_change && params.pen_change_prompt && !win) {
        post_status(_("当前无法弹出手动换笔确认窗口，请先使用有父窗口的绘图机工作台发送。"), true);
        finish_gcode_stream_ui();
        return;
    }

    std::thread([this, doc, desktop, selection, use_current_layer_without_selection, params, win]() mutable {
        std::lock_guard const guard(_port_mutex);
        auto const finish = [this] { finish_gcode_stream_ui(); };

        if (!_port || !_port->is_open()) {
            post_status(_("串口已断开。"), true);
            finish();
            return;
        }

        Inkscape::Axidraw::GrblExportContext ctx;
        ctx.desktop = desktop;
        ctx.selection = selection;
        ctx.use_current_layer_without_selection = use_current_layer_without_selection;
        ctx.cancel = &_gcode_cancel;

        auto pump = [] {
            if (auto const ctx = Glib::MainContext::get_default()) {
                while (ctx->iteration(false)) {
                }
            }
        };

        if (params.manual_pen_change && params.pen_change_prompt && win) {
            ctx.on_manual_pen_change_between_layers = [win](double, double) -> bool {
                Gtk::MessageDialog dlg(
                    *win,
                    _("下一层即将开始绘制。\n如果你按图层分笔/分颜色，请现在手动换笔，然后点击“是”继续。"),
                    true, Gtk::MessageType::QUESTION, Gtk::ButtonsType::YES_NO, true);
                dlg.set_secondary_text(
                    _("流程与 AxiDraw 手动换笔一致：先抬笔，可选回到原点，确认后再回到断点继续绘制。"));
                return Inkscape::UI::dialog_run(dlg) == Gtk::ResponseType::YES;
            };
        }

        struct StreamProgress {
            std::size_t stroke_done = 0;
            std::size_t stroke_total = 0;
            std::size_t lines_sent = 0;
            std::size_t lines_est = 0;
        };
        auto const progress = std::make_shared<StreamProgress>();
        using clock = std::chrono::steady_clock;
        auto const last_paint = std::make_shared<clock::time_point>(clock::time_point::min());
        auto const refresh_status = [this, progress, last_paint]() {
            if (progress->lines_est == 0 && progress->stroke_total == 0) {
                return;
            }
            auto const now = clock::now();
            constexpr auto k_min_interval = std::chrono::milliseconds(100);
            bool const at_end = (progress->lines_est > 0 && progress->lines_sent >= progress->lines_est) ||
                                (progress->stroke_total > 0 && progress->stroke_done >= progress->stroke_total);
            if (!at_end && now - *last_paint < k_min_interval) {
                return;
            }
            *last_paint = now;
            auto const status = make_direct_send_progress_status(progress->stroke_done, progress->stroke_total,
                                                                 progress->lines_sent, progress->lines_est);
            if (!status.empty()) {
                post_status(status, false);
            }
        };
        ctx.on_plot_stroke_progress = [progress, refresh_status](std::size_t done, std::size_t total) {
            progress->stroke_done = done;
            progress->stroke_total = total;
            refresh_status();
        };
        ctx.on_plot_gcode_line_progress = [progress, refresh_status](std::size_t sent, std::size_t est) {
            progress->lines_sent = sent;
            progress->lines_est = est;
            refresh_status();
        };

        grbl_begin_plot_waits(pump, &_gcode_cancel);
        scope_exit const end_plot{[] { grbl_end_plot_waits(); }};

        std::string err;
        std::size_t strokes = 0;
        GrblPlotStats stats{};
        if (!export_paths_to_grbl(*_port, doc, params, ctx, err, &strokes, &stats)) {
            post_gcode_stream_result(err);
            finish();
            return;
        }

        refresh_plot_feedback_after_gcode_change();
        post_status(make_direct_send_done_status(strokes, stats), false);
        finish();
    }).detach();
}

void GrblControlPanel::on_load_gcode_from_file()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    auto *win = get_dialog_parent_window("无法打开文件对话框（没有父窗口）。");
    if (!win) {
        return;
    }

    std::string folder;
    Inkscape::UI::Dialog::get_start_directory(folder, k_pref_save_gcode_dir, true);
    auto filters = create_gcode_file_filters();
    Glib::RefPtr<Gio::File> const src = choose_file_open(_("载入 G-code"), win, filters, folder, _("打开"));
    if (!src) {
        return;
    }
    std::string const path = src->get_path();
    if (path.empty()) {
        post_status(_("无法读取文件（没有本地路径）。"), true);
        return;
    }
    std::string contents;
    try {
        contents = Glib::file_get_contents(path);
    } catch (Glib::FileError const &e) {
        post_status(e.what(), true);
        return;
    }
    if (contents.size() > k_max_gcode_editor_bytes) {
        post_status(_("文件过大，无法载入编辑器。"), true);
        return;
    }
    if (auto const buf = _gcode_view.get_buffer()) {
        buf->set_text(contents);
    }
    if (auto *prefs = Inkscape::Preferences::get()) {
        prefs->setString(k_pref_save_gcode_dir, folder);
    }
    post_status(Glib::ustring::compose(_("已从“%1”载入 G-code"), src->get_parse_name()), false);
}

void GrblControlPanel::on_save_gcode_as()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    std::string text;
    if (!get_editor_gcode_text(text)) {
        return;
    }
    if (!std::any_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); })) {
        post_status(_("没有可保存内容（G-code 为空）。"), true);
        return;
    }
    auto *win = get_dialog_parent_window("无法打开保存对话框（没有父窗口）。");
    if (!win) {
        return;
    }

    std::string folder;
    Inkscape::UI::Dialog::get_start_directory(folder, k_pref_save_gcode_dir, true);
    auto filters = create_gcode_file_filters();

    std::string initial = "plot.nc";
    if (auto *d = getDocument()) {
        if (char const *fn = d->getDocumentFilename()) {
            std::string base = Glib::path_get_basename(fn);
            Inkscape::IO::remove_file_extension(base);
            if (!base.empty()) {
                initial = std::move(base) + ".nc";
            }
        }
    }

    Glib::RefPtr<Gio::File> const dest = choose_file_save(_("G-code 另存为"), win, filters, initial, folder);
    if (!dest) {
        return;
    }
    std::string const path = dest->get_path();
    if (path.empty()) {
        post_status(_("无法确定要保存到的本地文件路径。"), true);
        return;
    }
    try {
        Glib::file_set_contents(path, text);
    } catch (Glib::FileError const &e) {
        post_status(e.what(), true);
        return;
    }
    if (auto *prefs = Inkscape::Preferences::get()) {
        prefs->setString(k_pref_save_gcode_dir, folder);
    }
    post_status(Glib::ustring::compose(_("G-code 已保存到“%1”"), dest->get_parse_name()), false);
}

void GrblControlPanel::on_send_gcode()
{
    bool const send_from_cursor = _chk_send_from_cursor_line.get_active();
    guint editor_line_1 = 1;
    std::string text;
    if (!get_editor_gcode_text(text, send_from_cursor, &editor_line_1)) {
        return;
    }
    if (!std::any_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); })) {
        post_status(send_from_cursor ? Glib::ustring(_("从光标所在行往下没有可发送内容（为空或仅含注释）。"))
                                     : Glib::ustring(_("G-code 为空。")),
                    true);
        return;
    }
    std::size_t const total_exec = count_executable_gcode_lines(text);
    if (!begin_gcode_stream_ui({})) {
        return;
    }

    std::thread(
        [this, text = std::move(text), total_exec, send_from_cursor, editor_line_1]() mutable
        {
            std::lock_guard const guard(_port_mutex);
            auto const finish = [this] { finish_gcode_stream_ui(); };

            if (!link_is_open()) {
                post_status(_("尚未连接。"), true);
                finish();
                return;
            }

            auto pump = [] {
                if (auto const ctx = Glib::MainContext::get_default()) {
                    while (ctx->iteration(false)) {
                    }
                }
            };
            grbl_begin_plot_waits(pump, &_gcode_cancel);
            scope_exit const end_plot{[] { grbl_end_plot_waits(); }};

            if (send_from_cursor) {
                post_status(Glib::ustring::compose(_("正在从编辑器第 %1 行开始发送 G-code..."),
                                                   static_cast<guint64>(editor_line_1)),
                            false);
            }

            using clock = std::chrono::steady_clock;
            clock::time_point last_progress_wall = clock::now();
            std::size_t last_progress_at_sent = 0;
            auto try_send_progress = [&](std::size_t sent, bool force) {
                if (total_exec == 0) {
                    return;
                }
                clock::time_point const now = clock::now();
                std::size_t const span = sent - last_progress_at_sent;
                int const elapsed_ms =
                    static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_wall)
                                         .count());
                if (!force && sent != 1 && span < k_gcode_send_progress_line_stride &&
                    elapsed_ms < k_gcode_send_progress_min_interval_ms) {
                    return;
                }
                if (total_exec <= k_max_gcode_stream_lines) {
                    post_status(Glib::ustring::compose(_("正在发送 G-code：第 %1 / %2 行..."), static_cast<guint64>(sent),
                                                       static_cast<guint64>(total_exec)),
                                false);
                } else {
                    post_status(
                        Glib::ustring::compose(
                            _("正在发送 G-code：第 %1 行（程序超过 %2 行限制；发送将在报错时停止）..."),
                            static_cast<guint64>(sent), static_cast<guint64>(k_max_gcode_stream_lines)),
                        false);
                }
                last_progress_wall = now;
                last_progress_at_sent = sent;
            };

            std::string err;
            std::size_t sent = 0;
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                trim_in_place(line);
                if (should_skip_gcode_line(line)) {
                    continue;
                }
                if (sent >= k_max_gcode_stream_lines) {
                    err = _("G-code 行数过多（已超出限制）。");
                    break;
                }
                if (!link_write_line(line, err)) {
                    post_gcode_stream_result(err);
                    finish();
                    return;
                }
                ++sent;
                try_send_progress(sent, false);
            }
            if (!err.empty()) {
                post_gcode_stream_result(err);
            } else if (sent == 0) {
                post_status(_("没有可执行的行（只有空行或注释）。"), false);
            } else if (send_from_cursor) {
                post_status(Glib::ustring::compose(_("已发送 %1 行 G-code（起始于编辑器第 %2 行）。"),
                                                   static_cast<guint64>(sent), static_cast<guint64>(editor_line_1)),
                            false);
            } else {
                post_status(
                    Glib::ustring::compose(_("已发送 %1 行 G-code。"), static_cast<guint64>(sent)), false);
            }
            finish();
        })
        .detach();
}

void GrblControlPanel::build_ui()
{
    _jog_lbl.set_markup(_("<b>点动步长（mm）</b>"));
    for (char const *v : {"0.01", "0.1", "0.5", "1", "5", "10", "50", "100"}) {
        _jog_dist.append(v);
    }
    _jog_dist.set_active(3);

    _status.set_halign(Gtk::Align::START);
    _status.set_vexpand(false);
    _status.set_wrap(true);
    _status.set_max_width_chars(56);
    _status.set_selectable(true);
    _status.set_text(_("尚未连接。"));

    _btn_connect.set_label(_("连接"));
    _btn_connect.set_active(false);
    _btn_connect.set_tooltip_text(
        _("使用下方所选端口连接绘图机（也会保存到“编辑 -> 首选项 -> 输入/输出 -> 绘图机”）。"
          "波特率也来自同一页面。"));
    _radio_mode_combo.append("STA", _("WiFi 客户端（STA）"));
    _radio_mode_combo.append("AP", _("WiFi 热点（AP）"));
    _radio_mode_combo.append("BT", _("蓝牙（BT）"));
    _radio_mode_combo.append("OFF", _("关闭无线"));
    _radio_mode_combo.set_active_id("STA");
    _radio_mode_combo.set_hexpand(true);
    _radio_mode_combo.set_tooltip_text(
        _("通过 [ESP110] 设置 Grbl_ESP32 的无线模式。可选：STA / AP / BT / OFF。"));
    _radio_pwd.set_text("admin");
    _radio_pwd.set_visibility(false);
    _radio_pwd.set_placeholder_text(_("管理员密码"));
    _radio_pwd.set_hexpand(true);
    _radio_pwd.set_tooltip_text(
        _("[ESP110] 与可选的 [ESP444] 重启命令使用的管理员密码（默认通常是 admin）。"));
    _chk_radio_restart.set_label(_("切换模式后重启固件（[ESP444]）"));
    _chk_radio_restart.set_active(true);
    _chk_radio_restart.set_halign(Gtk::Align::START);
    _chk_radio_restart.set_tooltip_text(
        _("启用后，会在 [ESP110] 之后发送 [ESP444]RESTART，使无线模式立即生效。"));
    _btn_read_radio_mode.set_tooltip_text(
        _("通过 [ESP110]pwd=<password> 查询当前固件无线模式，并同步下拉框。"));
    _btn_apply_radio_mode.set_tooltip_text(
        _("向固件发送 [ESP110]<MODE>pwd=<password>。切换后可选触发 [ESP444]RESTART。"));

    _port_lbl.set_halign(Gtk::Align::START);
    _port_lbl.set_valign(Gtk::Align::CENTER);
    _port_lbl.set_markup(_("<b>Port</b>"));
    _port_combo.set_hexpand(true);
    _btn_refresh_ports.set_icon_name("view-refresh-symbolic");
    _btn_refresh_ports.set_tooltip_text(_("重新扫描串口"));
    _btn_refresh_ports.set_valign(Gtk::Align::CENTER);
    _btn_read_firmware.set_icon_name("document-properties-symbolic");
    _btn_read_firmware.set_tooltip_text(
        _("读取 $I / $G / $# / $$，并自动同步绘图机的方向反转与床面尺寸。"));
    _chk_sync_page_to_bed.set_halign(Gtk::Align::START);
    _chk_sync_page_to_bed.set_active(true);
    _chk_sync_page_to_bed.set_tooltip_text(
        _("启用后，如果固件返回了 $130/$131，就把当前文档页面尺寸同步成机器 X/Y 行程（单位 mm）。"));
    _machine_status.set_halign(Gtk::Align::START);
    _machine_status.set_ellipsize(Pango::EllipsizeMode::END);
    _machine_status.set_max_width_chars(56);
    _machine_status.add_css_class("monospace");
    _machine_status.set_tooltip_text(
        _("来自控制器的实时状态（连接期间大约每 1.5 秒轮询一次）。"));

    _frame.set_label(_("绘图机工作台"));
    _frame.set_margin_top(0);
    _frame.set_margin_bottom(0);
    _frame.set_margin_start(0);
    _frame.set_margin_end(0);

    _vbox.set_spacing(10);
    _vbox.set_margin_top(10);
    _vbox.set_margin_bottom(10);
    _vbox.set_margin_start(10);
    _vbox.set_margin_end(10);

    auto *grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(4);
    grid->set_column_spacing(4);
    grid->set_column_homogeneous(true);

    int r = 0;
    grid->attach(_btn_mech_home, 0, r, 1, 1);
    grid->attach(_btn_yp, 1, r, 1, 1);
    grid->attach(_btn_set_origin, 2, r, 1, 1);
    r++;
    grid->attach(_btn_xm, 0, r, 1, 1);
    grid->attach(_btn_goto_work_zero, 1, r, 1, 1);
    grid->attach(_btn_xp, 2, r, 1, 1);
    r++;
    grid->attach(_btn_reset, 0, r, 1, 1);
    grid->attach(_btn_ym, 1, r, 1, 1);
    r++;
    grid->attach(_jog_lbl, 0, r, 1, 1);
    grid->attach(_jog_dist, 1, r, 2, 1);
    r++;
    grid->attach(_btn_pen_up, 0, r, 1, 1);
    grid->attach(_btn_pen_down, 1, r, 1, 1);
    grid->attach(_btn_motors, 2, r, 1, 1);
    r++;
    grid->attach(_btn_clear_alarm, 0, r, 1, 1);

    for (auto *b : {&_btn_mech_home, &_btn_yp, &_btn_set_origin, &_btn_xm, &_btn_goto_work_zero, &_btn_xp, &_btn_reset, &_btn_ym,
                    &_btn_pen_up, &_btn_pen_down, &_btn_motors, &_btn_clear_alarm}) {
        b->set_hexpand(true);
    }
    _jog_dist.set_hexpand(true);

    _btn_mech_home.set_tooltip_text(_("回零：执行 $H（必须正确配置限位开关和安全间距）。"));
    _btn_yp.set_tooltip_text(_("Y 正向点动：先用相对模式 G1，再恢复为绝对模式 G90。"));
    _btn_set_origin.set_tooltip_text(_("G92：将当前位置设为工作零点（X0 Y0 Z0）。"));
    _btn_goto_work_zero.set_tooltip_text(_("G90 G0：以毫米单位（G21）快速移动到工作坐标 X0 Y0。"));
    _btn_goto_work_zero.set_icon_name("go-home-symbolic");
    _btn_xm.set_tooltip_text(_("按设定步长以毫米为单位向 X 负方向点动。"));
    _btn_xp.set_tooltip_text(_("按设定步长以毫米为单位向 X 正方向点动。"));
    _btn_ym.set_tooltip_text(_("按设定步长以毫米为单位向 Y 负方向点动。"));
    _btn_reset.set_tooltip_text(_("GRBL 软复位：发送 ASCII 0x18（Ctrl+X）。之后可能需要重新连接。"));
    _btn_reset.set_icon_name("view-refresh-symbolic");
    _btn_pen_up.set_tooltip_text(_("使用与绘图输出相同的抬笔方式（见 首选项 / GRBL）。"));
    _btn_pen_up.set_icon_name("go-up-symbolic");
    _btn_pen_down.set_tooltip_text(_("使用与绘图输出相同的落笔方式（见 首选项 / GRBL）。"));
    _btn_pen_down.set_icon_name("go-down-symbolic");
    _jog_dist.set_tooltip_text(_("X/Y 点动按钮使用的步长。"));
    _btn_fit_to_bed.set_tooltip_text(_("按当前机器床面宽/深，等比缩小整张图稿到行程内；如果本来就更小，则只移动到原点范围内。"));
    _btn_fit_to_bed.set_icon_name("transform-scale-symbolic");
    _btn_center_to_bed.set_tooltip_text(_("不改变图稿大小，只把整张图稿平移到当前机器行程的中心位置。"));
    _btn_center_to_bed.set_icon_name("align-horizontal-center-symbolic");
    _btn_restore_page_size.set_tooltip_text(_("如果页面曾被“连接/同步时把页面改成机器行程（可恢复）”改小，可用这里恢复到同步前的页面尺寸。"));
    _btn_restore_page_size.set_icon_name("edit-undo-symbolic");
    _layout_scale_summary.set_tooltip_text(_("这里会持续显示当前图稿相对机器行程的占用比例，以及“一键适配”后会缩放到多少。"));

    _chk_swap_xy.set_tooltip_text(_("将导出的机器坐标 X/Y 互换，适合机器坐标系相对画布旋转 90° 的情况。"));
    _chk_invert_x.set_tooltip_text(_("反转最终输出到机器的 X 坐标方向。"));
    _chk_invert_y.set_tooltip_text(_("反转最终输出到机器的 Y 坐标方向。"));
    _chk_flip_y.set_tooltip_text(_("按页面高度镜像 Y，用于把 SVG 画布的 Y 向下转换为机器常见的 Y 向上。"));
    _chk_align_origin.set_tooltip_text(_("将导出结果整体平移，使其左下角落在机器 X0 Y0。"));
    _chk_clip_bed.set_tooltip_text(_("将运动裁剪在床面范围内。超出部分会被截断。"));
    _chk_long_pen_up.set_tooltip_text(_("参考 kxnx 的绘图机策略：长距离空走前先抬到更高的位置，减少拖笔或蹭纸。"));
    _chk_near_connect.set_tooltip_text(_("参考 kxnx 的 nearDst 思路：如果相邻两段笔画距离很近，就直接连成一笔，减少抬笔和空走。启用后会实际画出这段连接线。"));
    _chk_sparse_sampling.set_tooltip_text(_("对规则排线或密集线场做快速抽稀：按笔画顺序每隔 N 条保留 1 条，减少发黑和绘制时间。"));
    _tool_change_mode_combo.append(k_tool_change_mode_none, _("不换笔"));
    _tool_change_mode_combo.append(k_tool_change_mode_manual, _("手动换笔"));
    _tool_change_mode_combo.append(k_tool_change_mode_m6, _("按图层工具号换笔(M6)"));
    _tool_change_mode_combo.set_active_id(k_tool_change_mode_none);
    _tool_change_mode_combo.set_tooltip_text(_("明确选择换笔模式：不换笔、手动换笔，或按图层名中的 T1/T2/T3 自动插入 Tn M6。"));
    _chk_manual_pen_change_to_home.set_tooltip_text(_("手动换笔前先回到 X0 Y0，便于取放画纸或人工换笔。"));
    _chk_manual_pen_change_prompt.set_tooltip_text(_("手动换笔时弹出确认提示，确认后再回到断点继续绘制。"));
    _chk_tool_change_point.set_tooltip_text(_("启用后，在发送 Tn M6 前先移动到固定换笔点。"));
    _bed_width_spin.set_digits(2);
    _bed_width_spin.set_range(1.0, 2000.0);
    _bed_width_spin.set_increments(1.0, 10.0);
    _bed_width_spin.set_tooltip_text(_("床面宽度（机器 X 方向，单位 mm）。"));
    _bed_depth_spin.set_digits(2);
    _bed_depth_spin.set_range(1.0, 2000.0);
    _bed_depth_spin.set_increments(1.0, 10.0);
    _bed_depth_spin.set_tooltip_text(_("床面深度（机器 Y 方向，单位 mm）。"));
    _long_pen_up_spin.set_digits(2);
    _long_pen_up_spin.set_range(-1000.0, 1000.0);
    _long_pen_up_spin.set_increments(0.5, 5.0);
    _long_pen_up_spin.set_tooltip_text(_("长距离空走时使用的抬笔高度/位置。"));
    _long_move_dist_spin.set_digits(2);
    _long_move_dist_spin.set_range(0.0, 100000.0);
    _long_move_dist_spin.set_increments(1.0, 10.0);
    _long_move_dist_spin.set_tooltip_text(_("当两段笔画之间的空走距离达到这个值时，触发高抬笔。"));
    _near_connect_dist_spin.set_digits(2);
    _near_connect_dist_spin.set_range(0.0, 1000.0);
    _near_connect_dist_spin.set_increments(0.05, 0.5);
    _near_connect_dist_spin.set_tooltip_text(_("当相邻两段笔画的间距不大于这个值时，直接不断笔连过去。单位 mm。"));
    _sparse_keep_every_spin.set_digits(0);
    _sparse_keep_every_spin.set_range(1.0, 64.0);
    _sparse_keep_every_spin.set_increments(1.0, 5.0);
    _sparse_keep_every_spin.set_tooltip_text(_("抽稀步长。1 表示不过滤，2 表示隔 1 条留 1 条，3 表示每 3 条保留 1 条。"));
    _tool_change_x_spin.set_digits(2);
    _tool_change_y_spin.set_digits(2);
    _tool_change_x_spin.set_range(-2000.0, 2000.0);
    _tool_change_y_spin.set_range(-2000.0, 2000.0);
    _tool_change_x_spin.set_increments(1.0, 10.0);
    _tool_change_y_spin.set_increments(1.0, 10.0);
    _tool_change_x_spin.set_tooltip_text(_("换笔点 X 坐标（mm）。"));
    _tool_change_y_spin.set_tooltip_text(_("换笔点 Y 坐标（mm）。"));
    _firmware_info_view.set_editable(false);
    _firmware_info_view.set_cursor_visible(false);
    _firmware_info_view.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    _firmware_info_view.add_css_class("monospace");
    if (auto const buf = _firmware_info_view.get_buffer()) {
        buf->set_text(_("尚未读取固件参数。"));
    }
    _firmware_info_scroll.set_child(_firmware_info_view);
    _firmware_info_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _firmware_info_scroll.set_min_content_height(100);
    _firmware_info_scroll.set_has_frame(true);

    _gcode_frame.set_label(_("绘图任务"));
    _gcode_help.set_markup(
        _("<small><b>从图稿填充</b> 会按与“发送文档到绘图机”相同的规则生成任务（包括选择集、图层限制和绘图机首选项），"
          "这样你可以在发送前先在这里检查或编辑。"
          "<b>载入 G-code</b> 会用文件内容替换编辑器；<b>G-code 另存为</b> 会把编辑器内容写入文件。"
          "<b>机器空间预览</b> 是主预览：它会把最终的毫米路径（经过镜像、原点偏移，以及可选的床面裁剪）以橙色映射回画布。"
          "<b>文档空间预览</b> 是机器坐标映射之前的可选参考几何。"
          "<b>仅从光标所在行向下发送</b> 会跳过光标以上的内容（适合发生错误后的续传）。"
          "每行一条指令；以 <tt>;</tt> 开头的行和整行 <tt>(...)</tt> 注释在发送时会被跳过。"
          "每发送一行，都会等待设备返回 <tt>ok</tt>。发送过程中，日志会显示进度。"
          "<b>取消</b> 会在当前行结束后生效。</small>"));
    _gcode_help.set_wrap(true);
    _gcode_help.set_halign(Gtk::Align::START);
    _gcode_help.set_margin_bottom(4);
    setup_summary_label(_job_summary, _("<b>任务概览</b>\n尚未分析当前图稿。"), 4, 4);
    auto *job_summary_frame = Gtk::make_managed<Gtk::Frame>();
    job_summary_frame->set_label(_("任务概览"));
    job_summary_frame->set_child(_job_summary);
    auto *job_tuning_grid = Gtk::make_managed<Gtk::Grid>();
    job_tuning_grid->set_row_spacing(4);
    job_tuning_grid->set_column_spacing(8);
    job_tuning_grid->attach(_chk_near_connect, 0, 0, 1, 1);
    auto *lbl_near_connect = Gtk::make_managed<Gtk::Label>(_("连笔距离(mm)"), Gtk::Align::START);
    job_tuning_grid->attach(*lbl_near_connect, 1, 0, 1, 1);
    job_tuning_grid->attach(_near_connect_dist_spin, 2, 0, 1, 1);
    job_tuning_grid->attach(_chk_sparse_sampling, 0, 1, 1, 1);
    auto *lbl_sparse_sampling = Gtk::make_managed<Gtk::Label>(_("每隔 N 条留 1 条"), Gtk::Align::START);
    job_tuning_grid->attach(*lbl_sparse_sampling, 1, 1, 1, 1);
    job_tuning_grid->attach(_sparse_keep_every_spin, 2, 1, 1, 1);
    auto *lbl_tool_change_mode = Gtk::make_managed<Gtk::Label>(_("换笔模式"), Gtk::Align::START);
    job_tuning_grid->attach(*lbl_tool_change_mode, 0, 2, 1, 1);
    job_tuning_grid->attach(_tool_change_mode_combo, 1, 2, 2, 1);
    job_tuning_grid->attach(_chk_manual_pen_change_to_home, 0, 3, 2, 1);
    job_tuning_grid->attach(_chk_manual_pen_change_prompt, 0, 4, 2, 1);
    job_tuning_grid->attach(_chk_tool_change_point, 0, 5, 1, 1);
    auto *tool_change_xy_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto *lbl_tool_change_x = Gtk::make_managed<Gtk::Label>(_("X(mm)"), Gtk::Align::START);
    auto *lbl_tool_change_y = Gtk::make_managed<Gtk::Label>(_("Y(mm)"), Gtk::Align::START);
    tool_change_xy_box->append(*lbl_tool_change_x);
    tool_change_xy_box->append(_tool_change_x_spin);
    tool_change_xy_box->append(*lbl_tool_change_y);
    tool_change_xy_box->append(_tool_change_y_spin);
    job_tuning_grid->attach(*tool_change_xy_box, 1, 5, 2, 1);
    auto *job_tuning_hint = Gtk::make_managed<Gtk::Label>(
        _("<small>“近距离自动连笔”会把非常接近的相邻笔画合并成连续路径，减少抬笔和空走，但也会真的画出连接线。适合绘图机轮廓、描边类任务；如果不希望出现桥接线，请关闭。"
          "“排线抽稀”适合规则排线、阴影线、Sparse 图，按当前笔画顺序隔线保留，可显著减少发黑和总时长。"
          "换笔请明确选一种模式："
          "“手动换笔”会在分层切换时暂停，等你人工处理后再继续；"
          "“按图层工具号换笔(M6)”会读取图层名中的 T1/T2/T3，在层切换时插入 Tn M6，适合支持半自动换笔流程的固件。</small>"),
        Gtk::Align::START);
    job_tuning_hint->set_use_markup(true);
    job_tuning_hint->set_wrap(true);
    auto *start_gcode_lbl = Gtk::make_managed<Gtk::Label>(_("起始 G-code"), Gtk::Align::START);
    auto *end_gcode_lbl = Gtk::make_managed<Gtk::Label>(_("结束 G-code"), Gtk::Align::START);
    start_gcode_lbl->set_tooltip_text(_("在 G21/G90 之后、正式开始绘图之前插入的自定义指令。一行一条。"));
    end_gcode_lbl->set_tooltip_text(_("在最终抬笔之后插入的自定义指令。一行一条。"));
    _start_gcode_view.add_css_class("monospace");
    _end_gcode_view.add_css_class("monospace");
    _start_gcode_view.set_wrap_mode(Gtk::WrapMode::NONE);
    _end_gcode_view.set_wrap_mode(Gtk::WrapMode::NONE);
    _start_gcode_view.set_top_margin(4);
    _start_gcode_view.set_bottom_margin(4);
    _start_gcode_view.set_left_margin(4);
    _start_gcode_view.set_right_margin(4);
    _end_gcode_view.set_top_margin(4);
    _end_gcode_view.set_bottom_margin(4);
    _end_gcode_view.set_left_margin(4);
    _end_gcode_view.set_right_margin(4);
    _start_gcode_scroll.set_child(_start_gcode_view);
    _end_gcode_scroll.set_child(_end_gcode_view);
    _start_gcode_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _end_gcode_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _start_gcode_scroll.set_min_content_height(56);
    _end_gcode_scroll.set_min_content_height(56);
    _start_gcode_scroll.set_has_frame(true);
    _end_gcode_scroll.set_has_frame(true);
    _start_gcode_view.set_tooltip_text(_("例如：M117 Plot Start、G0 X0 Y0 等。一行一条；空行会忽略。"));
    _end_gcode_view.set_tooltip_text(_("例如：G0 X0 Y0、M84、M117 Plot Done 等。一行一条；空行会忽略。"));
    _chk_canvas_plot_preview.set_tooltip_text(
        _("可选的文档空间参考叠加层：使用与导出时相同的采样和笔画顺序，但发生在毫米换算、页面 Y 镜像、"
          "将绘图原点平移到 X0 Y0、以及机器床面裁剪之前。可用来对比原始几何与最终的机器空间预览。"));
    _chk_canvas_plot_preview.set_halign(Gtk::Align::START);
    _chk_machine_space_preview.set_tooltip_text(
        _("主预览叠加层：显示与最终 G-code 相同的折线路径，已完成毫米换算、可选页面 Y 镜像、"
          "将绘图原点平移到 X0 Y0，以及可选机器床面裁剪，然后再映射回文档单位。"
          "如果床面裁剪截掉了部分笔画，则这些区域的预览会是近似结果。"));
    _chk_machine_space_preview.set_halign(Gtk::Align::START);
    _chk_machine_space_preview.set_active(true);
    _chk_canvas_plot_preview.set_active(false);
    _chk_send_from_cursor_line.set_tooltip_text(
        _("启用后，“发送到机器”只会从文本光标所在行的开头一直发送到编辑器末尾。"
          "如果 Grbl 执行中报错，你可以删除或跳过已执行的行，然后把光标放到下一条命令处继续发送。"));
    _chk_send_from_cursor_line.set_halign(Gtk::Align::START);
    _gcode_view.set_accepts_tab(false);
    if (auto const buf = _gcode_view.get_buffer()) {
        buf->set_text("");
    }
    _gcode_view.add_css_class("monospace");
    _gcode_view.set_top_margin(4);
    _gcode_view.set_bottom_margin(4);
    _gcode_view.set_left_margin(4);
    _gcode_view.set_right_margin(4);
    _gcode_view.set_vexpand(true);
    _gcode_scroll.set_child(_gcode_view);
    _gcode_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _gcode_scroll.set_vexpand(true);
    _gcode_scroll.set_min_content_height(120);
    _gcode_scroll.set_has_frame(true);

    _btn_cancel_gcode.set_sensitive(false);
    _btn_load_gcode.set_icon_name("document-open-symbolic");
    _btn_fill_from_drawing.set_icon_name("document-properties-symbolic");
    _btn_send_from_drawing.set_icon_name("media-playback-start-symbolic");
    _btn_save_gcode.set_icon_name("document-save-as-symbolic");
    _btn_send_gcode.set_icon_name("document-send-symbolic");
    _btn_cancel_gcode.set_icon_name("process-stop-symbolic");
    _btn_load_gcode.set_tooltip_text(_("从文本文件（UTF-8）替换编辑器内容，大小上限与生成任务相同。"));
    _btn_fill_from_drawing.set_tooltip_text(
        _("按当前绘图机首选项从当前文档生成 G-code（规则与主菜单中的“发送文档到绘图机”一致）。"));
    _btn_send_from_drawing.set_label(_("从图稿直接发送"));
    _btn_send_from_drawing.set_tooltip_text(
        _("按当前绘图机首选项直接从当前文档生成 G-code，并立刻发送到已连接的绘图机。"));
    _btn_save_gcode.set_tooltip_text(_("将编辑器中的文本保存为 .nc / .gcode 文件（UTF-8）。"));
    _btn_send_gcode.set_tooltip_text(
        _("按顺序发送每一条非空行，并在发送下一行前等待 Grbl 返回 ok（或错误）。"
          "长任务执行时，消息日志会更新大致的行数进度。"
          "也可以只从光标所在行开始发送（见上方复选框）。"));
    _btn_cancel_gcode.set_tooltip_text(
        _("设置取消标记；当前这一行仍可能执行完后才会停止发送。"));

    Inkscape::UI::pack_start(_gcode_inner, _gcode_help, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, *job_tuning_grid, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, *job_tuning_hint, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, *start_gcode_lbl, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _start_gcode_scroll, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, *end_gcode_lbl, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _end_gcode_scroll, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_canvas_plot_preview, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_machine_space_preview, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_send_from_cursor_line, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _gcode_scroll, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_load_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_fill_from_drawing, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_send_from_drawing, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_save_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_send_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_cancel_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_inner, _gcode_actions, false, false, 2);
    _gcode_frame.set_child(_gcode_inner);

    Inkscape::UI::pack_start(_port_row, _port_lbl, false, false, 6);
    Inkscape::UI::pack_start(_port_row, _port_combo, true, true, 6);
    Inkscape::UI::pack_start(_port_row, _btn_refresh_ports, false, false, 0);
    Inkscape::UI::pack_start(_port_row, _btn_read_firmware, false, false, 0);

    auto *frame_layout = Gtk::make_managed<Gtk::Frame>();
    frame_layout->set_label(_("页面、机器与恢复"));
    frame_layout->set_margin_top(0);
    auto *box_layout = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    auto *layout_hint = Gtk::make_managed<Gtk::Label>(
        _("<small>这里可以按机器行程整理图稿，也可以在页面被同步改小后恢复原页面尺寸；画布预览中会标出机器原点、机器 X+、机器 Y+ 方向。</small>"),
        Gtk::Align::START);
    layout_hint->set_use_markup(true);
    layout_hint->set_wrap(true);
    auto *top_actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    _btn_fit_to_bed.set_hexpand(true);
    _btn_center_to_bed.set_hexpand(true);
    _btn_restore_page_size.set_hexpand(true);
    top_actions->append(_btn_fit_to_bed);
    top_actions->append(_btn_center_to_bed);
    top_actions->append(_btn_restore_page_size);
    setup_summary_label(_layout_scale_summary, _("<b>当前缩放</b>\n尚未分析当前图稿与机器行程。"), 2, 2);
    update_page_restore_button();
    Inkscape::UI::pack_start(*box_layout, *top_actions, false, false, 0);
    Inkscape::UI::pack_start(*box_layout, _layout_scale_summary, false, false, 0);
    Inkscape::UI::pack_start(*box_layout, _chk_sync_page_to_bed, false, false, 0);
    Inkscape::UI::pack_start(*box_layout, *layout_hint, false, false, 0);
    frame_layout->set_child(*box_layout);

    auto *frame_serial = Gtk::make_managed<Gtk::Frame>();
    frame_serial->set_label(_("绘图机连接"));
    frame_serial->set_margin_top(0);
    auto *box_serial = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    Inkscape::UI::pack_start(*box_serial, _port_row, false, false, 0);
    auto *radio_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto *radio_lbl = Gtk::make_managed<Gtk::Label>(_("<b>无线与固件</b>"), Gtk::Align::START);
    radio_lbl->set_use_markup(true);
    Inkscape::UI::pack_start(*radio_row, *radio_lbl, false, false, 0);
    Inkscape::UI::pack_start(*radio_row, _radio_mode_combo, true, true, 0);
    Inkscape::UI::pack_start(*radio_row, _radio_pwd, true, true, 0);
    Inkscape::UI::pack_start(*radio_row, _btn_read_radio_mode, false, false, 0);
    Inkscape::UI::pack_start(*radio_row, _btn_apply_radio_mode, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, *radio_row, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _chk_radio_restart, false, false, 0);
    auto *hdr_status = Gtk::make_managed<Gtk::Label>();
    hdr_status->set_markup(_("<small>绘图机状态</small>"));
    hdr_status->set_halign(Gtk::Align::START);
    Inkscape::UI::pack_start(*box_serial, *hdr_status, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _machine_status, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _btn_connect, false, false, 0);
    frame_serial->set_child(*box_serial);

    auto *frame_mapping = Gtk::make_managed<Gtk::Frame>();
    frame_mapping->set_label(_("绘图范围与坐标映射"));
    auto *box_mapping = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto *mapping_grid = Gtk::make_managed<Gtk::Grid>();
    mapping_grid->set_row_spacing(4);
    mapping_grid->set_column_spacing(8);
    mapping_grid->attach(_chk_swap_xy, 0, 0, 1, 1);
    mapping_grid->attach(_chk_invert_x, 1, 0, 1, 1);
    mapping_grid->attach(_chk_invert_y, 2, 0, 1, 1);
    mapping_grid->attach(_chk_flip_y, 0, 1, 2, 1);
    mapping_grid->attach(_chk_align_origin, 2, 1, 1, 1);
    mapping_grid->attach(_chk_clip_bed, 0, 2, 1, 1);
    auto *lbl_bed_w = Gtk::make_managed<Gtk::Label>(_("床面宽(mm)"), Gtk::Align::START);
    auto *lbl_bed_d = Gtk::make_managed<Gtk::Label>(_("床面深(mm)"), Gtk::Align::START);
    mapping_grid->attach(*lbl_bed_w, 1, 2, 1, 1);
    mapping_grid->attach(_bed_width_spin, 2, 2, 1, 1);
    mapping_grid->attach(*lbl_bed_d, 1, 3, 1, 1);
    mapping_grid->attach(_bed_depth_spin, 2, 3, 1, 1);
    mapping_grid->attach(_chk_long_pen_up, 0, 4, 1, 1);
    auto *lbl_long_pen = Gtk::make_managed<Gtk::Label>(_("高抬笔位置"), Gtk::Align::START);
    auto *lbl_long_move = Gtk::make_managed<Gtk::Label>(_("触发距离(mm)"), Gtk::Align::START);
    mapping_grid->attach(*lbl_long_pen, 1, 4, 1, 1);
    mapping_grid->attach(_long_pen_up_spin, 2, 4, 1, 1);
    mapping_grid->attach(*lbl_long_move, 1, 5, 1, 1);
    mapping_grid->attach(_long_move_dist_spin, 2, 5, 1, 1);
    auto *mapping_hint = Gtk::make_managed<Gtk::Label>(
        _("<small>“同步绘图机参数”会把 $3 同步到反转 X/Y，把 $130/$131 同步到床面尺寸。交换 X/Y 属于主机侧映射，需要你按绘图机结构手动设置。长距离高抬笔参考了 kxnx 绘图机软件中的做法；近距离连笔与起止 G-code 放在下方“绘图任务”区域统一设置。</small>"),
        Gtk::Align::START);
    mapping_hint->set_use_markup(true);
    mapping_hint->set_wrap(true);
    Inkscape::UI::pack_start(*box_mapping, *mapping_grid, false, false, 0);
    Inkscape::UI::pack_start(*box_mapping, *mapping_hint, false, false, 0);
    Inkscape::UI::pack_start(*box_mapping, _firmware_info_scroll, true, true, 0);
    frame_mapping->set_child(*box_mapping);

    auto *frame_motion = Gtk::make_managed<Gtk::Frame>();
    frame_motion->set_label(_("走笔与点动"));
    auto *box_motion = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    box_motion->append(*grid);
    frame_motion->set_child(*box_motion);

    auto *frame_log = Gtk::make_managed<Gtk::Frame>();
    frame_log->set_label(_("日志"));
    auto *box_log = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto *hdr_log = Gtk::make_managed<Gtk::Label>();
    hdr_log->set_markup(_("<small>消息</small>"));
    hdr_log->set_halign(Gtk::Align::START);
    Inkscape::UI::pack_start(*box_log, *hdr_log, false, false, 0);
    Inkscape::UI::pack_start(*box_log, _status, true, true, 0);
    frame_log->set_child(*box_log);

    Inkscape::UI::pack_start(_vbox, *frame_layout, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *job_summary_frame, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *frame_serial, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *frame_mapping, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *frame_motion, false, false, 0);
    Inkscape::UI::pack_start(_vbox, _gcode_frame, true, true, 0);
    Inkscape::UI::pack_start(_vbox, *frame_log, false, false, 0);
    _frame.set_child(_vbox);
    append(_frame);

    load_mapping_preferences_to_ui();

    _btn_connect.signal_toggled().connect(sigc::mem_fun(*this, &GrblControlPanel::connect_toggle));
    _btn_refresh_ports.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::refresh_port_list));
    _btn_read_firmware.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_read_firmware_settings));
    _port_combo.signal_changed().connect(sigc::mem_fun(*this, &GrblControlPanel::on_port_combo_changed));
    _chk_swap_xy.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_invert_x.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_invert_y.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_flip_y.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_align_origin.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_clip_bed.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_long_pen_up.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_near_connect.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_sparse_sampling.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _tool_change_mode_combo.signal_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_manual_pen_change_to_home.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_manual_pen_change_prompt.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _chk_tool_change_point.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _bed_width_spin.signal_value_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    _bed_depth_spin.signal_value_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    _long_pen_up_spin.signal_value_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    _long_move_dist_spin.signal_value_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    _near_connect_dist_spin.signal_value_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    _sparse_keep_every_spin.signal_value_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    _tool_change_x_spin.signal_value_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    _tool_change_y_spin.signal_value_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    if (auto const buf = _start_gcode_view.get_buffer()) {
        buf->signal_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    }
    if (auto const buf = _end_gcode_view.get_buffer()) {
        buf->signal_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    }
    _btn_read_radio_mode.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            std::string const pwd = _radio_pwd.get_text();
            if (pwd.empty()) {
                e = _("管理员密码为空。");
                return;
            }
            std::string const cmd = "[ESP110]pwd=" + pwd + "\n";
            if (!link_write_bytes(cmd.data(), cmd.size())) {
                e = _("无法发送无线模式查询命令。");
                return;
            }
            std::string reply;
            for (int i = 0; i < 8; ++i) {
                std::string line;
                if (!link_read_line(line, 1200)) {
                    break;
                }
                trim_in_place(line);
                if (line.empty() || line == "ok") {
                    continue;
                }
                if (line.rfind("error", 0) == 0 || line.rfind("ERROR", 0) == 0) {
                    e = line;
                    return;
                }
                reply = line;
                break;
            }
            if (reply.empty()) {
                e = _("固件没有返回无线模式信息。");
                return;
            }
            std::string const mode = detect_radio_mode_from_reply(reply);
            if (mode.empty()) {
                post_status(
                    Glib::ustring::compose(_("已收到无线模式回复，但无法识别：%1"),
                                           Glib::ustring(reply)),
                    true);
                return;
            }
            Glib::signal_idle().connect_once(sigc::track_object([this, mode] {
                _radio_mode_combo.set_active_id(mode);
            }, *this));
            post_status(Glib::ustring::compose(_("当前固件无线模式：%1"), Glib::ustring(mode)), false);
        }, false);
    });
    _btn_apply_radio_mode.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            Glib::ustring mode = _radio_mode_combo.get_active_id();
            if (mode.empty()) {
                mode = "STA";
            }
            std::string const pwd = _radio_pwd.get_text();
            if (pwd.empty()) {
                e = _("管理员密码为空。");
                return;
            }
            std::string cmd = "[ESP110]" + mode.raw() + "pwd=" + pwd;
            if (!link_write_line(cmd, e)) {
                return;
            }
            if (_chk_radio_restart.get_active()) {
                std::string restart_cmd = "[ESP444]RESTART pwd=" + pwd;
                std::string restart_err;
                if (!link_write_line(restart_cmd, restart_err)) {
                    post_status(
                        Glib::ustring::compose(
                            _("无线模式命令已发送，但重启命令失败：%1。你可以手动重新连接。"),
                            Glib::ustring(restart_err)),
                        true);
                    return;
                }
            }
            Glib::ustring reconnect_hint;
            if (mode == "BT") {
                reconnect_hint = _("请将主机连接切换到蓝牙后重新连接。");
            } else if (mode == "AP") {
                reconnect_hint = _("请连接到控制器的 AP，然后使用其 AP IP 地址（通常是 192.168.0.1 或你配置的值）。");
            } else if (mode == "STA") {
                reconnect_hint = _("请使用控制器在 STA 模式下的 IP/主机名通过局域网重新连接。");
            } else {
                reconnect_hint = _("无线已关闭；请改用有线串口重新连接。");
            }
            post_status(
                Glib::ustring::compose(
                    _("无线模式命令已发送：%1。%2"),
                    mode, reconnect_hint),
                false);
        }, false);
    });

    _btn_mech_home.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!link_write_line("$H", e)) {
                return;
            }
        });
    });
    _btn_yp.signal_clicked().connect([this] { jog_y(+1.0); });
    _btn_ym.signal_clicked().connect([this] { jog_y(-1.0); });
    _btn_xp.signal_clicked().connect([this] { jog_x(+1.0); });
    _btn_xm.signal_clicked().connect([this] { jog_x(-1.0); });
    _btn_set_origin.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!link_write_line("G21", e)) {
                return;
            }
            if (!link_write_line("G92 X0 Y0 Z0", e)) {
                return;
            }
        });
    });
    _btn_goto_work_zero.signal_clicked().connect([this] {
        run_action(
            [this](std::string &e) {
                if (!link_write_line("G21", e)) {
                    return;
                }
                if (!link_write_line("G90", e)) {
                    return;
                }
                if (!link_write_line("G0 X0 Y0", e)) {
                    return;
                }
            });
    });
    _btn_reset.signal_clicked().connect([this] { soft_reset(); });
    _btn_pen_up.signal_clicked().connect([this] { send_pen_state(true); });
    _btn_pen_down.signal_clicked().connect([this] { send_pen_state(false); });
    _btn_motors.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!link_write_line("$SLP", e)) {
                return;
            }
        });
    });
    _btn_clear_alarm.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!link_write_line("$X", e)) {
                return;
            }
        });
    });
    _chk_canvas_plot_preview.signal_toggled().connect([this] { schedule_plot_feedback_refresh(true); });
    _chk_machine_space_preview.signal_toggled().connect([this] { schedule_plot_feedback_refresh(true); });
    _chk_sync_page_to_bed.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _btn_fit_to_bed.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_fit_document_to_bed));
    _btn_center_to_bed.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_center_document_to_bed));
    _btn_restore_page_size.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_restore_page_size));
    _btn_load_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_load_gcode_from_file));
    _btn_fill_from_drawing.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_fill_gcode_from_document));
    _btn_send_from_drawing.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_send_document_direct));
    _btn_save_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_save_gcode_as));
    _btn_send_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_send_gcode));
    _btn_cancel_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_cancel_gcode_stream));
}

} // namespace Inkscape::UI::Dialog

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
