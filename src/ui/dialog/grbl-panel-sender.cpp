// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-sender.h"

#include "grbl-control-panel.h"

#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

#include <glibmm/i18n.h>
#include <glibmm/ustring.h>
#include <gtkmm/messagedialog.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/grbl-link.h"
#include "axidraw/device/serial-port.h"
#include "axidraw/pipeline/grbl-export.h"
#include "ui/dialog-run.h"

namespace Inkscape::UI::Dialog {
namespace {

constexpr int k_gcode_send_progress_min_interval_ms = 350;
constexpr std::size_t k_gcode_send_progress_line_stride = 80;
constexpr std::size_t k_max_gcode_stream_lines = 200000;

template <typename Func>
void for_each_executable_gcode_line(std::string const &text, Func &&func)
{
    auto trim_in_place = [](std::string &s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
            s.pop_back();
        }
        auto it = s.begin();
        while (it != s.end() && (*it == ' ' || *it == '\t')) {
            ++it;
        }
        s.erase(s.begin(), it);
    };
    auto should_skip_gcode_line = [](std::string const &s) {
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
    };

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
        func(line);
    }
}

bool get_air_travel_ratio_text(Inkscape::Axidraw::GrblPlotStats const &stats, Glib::ustring &ratio_out)
{
    if (!stats.has_length_stats) {
        return false;
    }
    auto const total = stats.draw_length_mm + stats.travel_length_mm;
    if (!(total > 1e-9)) {
        return false;
    }
    auto const ratio = (stats.travel_length_mm / total) * 100.0;
    ratio_out = Glib::ustring::format(std::fixed, std::setprecision(1), ratio);
    return true;
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
    Glib::ustring ratio;
    if (get_air_travel_ratio_text(stats, ratio)) {
        return Glib::ustring::compose(_("图稿直发完成：共发送 %1 条笔画，空走占比约 %2%%。"),
                                      static_cast<guint64>(strokes), ratio);
    }
    return Glib::ustring::compose(_("图稿直发完成：共发送 %1 条笔画。"), static_cast<guint64>(strokes));
}

struct GcodeSendProgressTracker
{
    using clock = std::chrono::steady_clock;

    std::function<void(Glib::ustring const &, bool)> post_status;
    std::size_t total_exec = 0;
    clock::time_point last_progress_wall = clock::now();
    std::size_t last_progress_at_sent = 0;

    void maybe_post(std::size_t sent, bool force = false)
    {
        if (total_exec == 0) {
            return;
        }
        auto const now = clock::now();
        auto const span = sent - last_progress_at_sent;
        int const elapsed_ms =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_wall).count());
        if (!force && sent != 1 && span < k_gcode_send_progress_line_stride &&
            elapsed_ms < k_gcode_send_progress_min_interval_ms) {
            return;
        }
        if (total_exec <= k_max_gcode_stream_lines) {
            post_status(
                Glib::ustring::compose(_("正在发送 G-code：第 %1 / %2 行..."), static_cast<guint64>(sent),
                                       static_cast<guint64>(total_exec)),
                false);
        } else {
            post_status(
                Glib::ustring::compose(_("正在发送 G-code：第 %1 行（程序超过 %2 行限制；发送将在报错时停止）..."),
                                       static_cast<guint64>(sent), static_cast<guint64>(k_max_gcode_stream_lines)),
                false);
        }
        last_progress_wall = now;
        last_progress_at_sent = sent;
    }
};

void post_gcode_send_completion(std::function<void(Glib::ustring const &, bool)> const &post_status,
                                bool send_from_cursor, guint editor_line_1, std::size_t sent)
{
    if (sent == 0) {
        post_status(_("没有可执行的行（只有空行或注释）。"), false);
    } else if (send_from_cursor) {
        post_status(Glib::ustring::compose(_("已发送 %1 行 G-code（起始于编辑器第 %2 行）。"),
                                           static_cast<guint64>(sent), static_cast<guint64>(editor_line_1)),
                    false);
    } else {
        post_status(Glib::ustring::compose(_("已发送 %1 行 G-code。"), static_cast<guint64>(sent)), false);
    }
}

Inkscape::Axidraw::GrblExportContext make_direct_send_context(SPDesktop *desktop, Inkscape::Selection *selection,
                                                              bool use_current_layer_without_selection,
                                                              std::atomic<bool> const *cancel, Gtk::Window *win)
{
    Inkscape::Axidraw::GrblExportContext ctx;
    ctx.desktop = desktop;
    ctx.selection = selection;
    ctx.use_current_layer_without_selection = use_current_layer_without_selection;
    ctx.cancel = cancel;

    if (win) {
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

    return ctx;
}

void attach_direct_send_progress_callbacks(std::function<void(Glib::ustring const &, bool)> const &post_status,
                                           Inkscape::Axidraw::GrblExportContext &ctx)
{
    struct StreamProgress {
        std::size_t stroke_done = 0;
        std::size_t stroke_total = 0;
        std::size_t lines_sent = 0;
        std::size_t lines_est = 0;
    };

    auto const progress = std::make_shared<StreamProgress>();
    using clock = std::chrono::steady_clock;
    auto const last_paint = std::make_shared<clock::time_point>(clock::time_point::min());
    auto const refresh_status = [post_status, progress, last_paint]() {
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
}

} // namespace

void GrblPanelSender::run_direct_send_worker(GrblControlPanel &panel, std::unique_lock<std::mutex> &port_lock,
                                             SPDocument *doc, SPDesktop *desktop, Inkscape::Selection *selection,
                                             bool const use_current_layer_without_selection,
                                             Inkscape::Axidraw::GrblExportParams params, Gtk::Window *win)
{
    auto const finish = [&panel, &port_lock] { panel.finish_gcode_stream_worker(port_lock); };

    auto *serial = panel._link ? panel._link->serial_port() : nullptr;
    if (!serial || !serial->is_open()) {
        panel.post_status(_("串口已断开。"), true);
        finish();
        return;
    }

    auto ctx = make_direct_send_context(desktop, selection, use_current_layer_without_selection, &panel._gcode_cancel,
                                        params.manual_pen_change && params.pen_change_prompt ? win : nullptr);
    attach_direct_send_progress_callbacks(
        [&panel](Glib::ustring const &status, bool is_error) { panel.post_status(status, is_error); }, ctx);

    panel.with_grbl_plot_waits([&] {
        std::string err;
        std::size_t strokes = 0;
        Inkscape::Axidraw::GrblPlotStats stats{};
        if (!Inkscape::Axidraw::export_paths_to_grbl(*serial, doc, params, ctx, err, &strokes, &stats)) {
            panel.post_gcode_stream_result(err);
            return;
        }

        panel.refresh_plot_feedback_after_gcode_change();
        panel.post_status(make_direct_send_done_status(strokes, stats), false);
    });
    finish();
}

void GrblPanelSender::run_editor_gcode_send_worker(GrblControlPanel &panel, std::unique_lock<std::mutex> &port_lock,
                                                   std::string text, std::size_t const total_exec,
                                                   bool const send_from_cursor, guint const editor_line_1)
{
    auto const finish = [&panel, &port_lock] { panel.finish_gcode_stream_worker(port_lock); };

    if (!(panel._link && panel._link->is_open())) {
        panel.post_not_connected_status();
        finish();
        return;
    }

    if (send_from_cursor) {
        panel.post_status(Glib::ustring::compose(_("正在从编辑器第 %1 行开始发送 G-code..."),
                                                 static_cast<guint64>(editor_line_1)),
                          false);
    }

    GcodeSendProgressTracker progress{
        .post_status = [&panel](Glib::ustring const &status, bool is_error) { panel.post_status(status, is_error); },
        .total_exec = total_exec,
    };

    panel.with_grbl_plot_waits([&] {
        std::string err;
        bool write_failed = false;
        std::size_t sent = 0;
        for_each_executable_gcode_line(text, [&](std::string const &line) {
            if (!err.empty() || write_failed) {
                return;
            }
            if (sent >= k_max_gcode_stream_lines) {
                err = _("G-code 行数过多（已超出限制）。");
                return;
            }
            if (!panel._link->send_line_wait_ok(line, err)) {
                write_failed = true;
                return;
            }
            ++sent;
            progress.maybe_post(sent, false);
        });
        if (write_failed) {
            panel.post_gcode_stream_result(err);
            return;
        }
        if (!err.empty()) {
            panel.post_gcode_stream_result(err);
        } else {
            post_gcode_send_completion(
                [&panel](Glib::ustring const &status, bool is_error) { panel.post_status(status, is_error); },
                send_from_cursor, editor_line_1, sent);
        }
    });
    finish();
}

} // namespace Inkscape::UI::Dialog
