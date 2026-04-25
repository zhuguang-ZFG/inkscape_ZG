// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-sender.h"

#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

#include <glibmm/i18n.h>
#include <glibmm/ustring.h>
#include <gtkmm/messagedialog.h>

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
        if (s.empty() || s[0] == ';') {
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

Glib::ustring make_direct_send_progress_status(std::size_t stroke_done, std::size_t stroke_total,
                                               std::size_t lines_sent, std::size_t lines_est)
{
    if (lines_est > 0 && lines_sent > 0 && stroke_total > 0 && stroke_done > 0) {
        return Glib::ustring::compose(_("Sending drawing: stroke %1 / %2, line %3 / ~%4..."),
                                      static_cast<guint64>(stroke_done), static_cast<guint64>(stroke_total),
                                      static_cast<guint64>(lines_sent), static_cast<guint64>(lines_est));
    }
    if (lines_est > 0 && lines_sent > 0) {
        return Glib::ustring::compose(_("Sending drawing: line %1 / ~%2..."),
                                      static_cast<guint64>(lines_sent), static_cast<guint64>(lines_est));
    }
    if (stroke_total > 0 && stroke_done > 0) {
        return Glib::ustring::compose(_("Sending drawing: stroke %1 / %2..."),
                                      static_cast<guint64>(stroke_done), static_cast<guint64>(stroke_total));
    }
    return {};
}

Glib::ustring make_direct_send_done_status(std::size_t strokes, Inkscape::Axidraw::GrblPlotStats const &stats)
{
    Glib::ustring ratio;
    if (get_air_travel_ratio_text(stats, ratio)) {
        return Glib::ustring::compose(_("Direct send completed: %1 strokes sent, air-travel ratio about %2%%."),
                                      static_cast<guint64>(strokes), ratio);
    }
    return Glib::ustring::compose(_("Direct send completed: %1 strokes sent."), static_cast<guint64>(strokes));
}

struct GcodeSendProgressTracker {
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
        int const elapsed_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_wall).count());
        if (!force && sent != 1 && span < k_gcode_send_progress_line_stride &&
            elapsed_ms < k_gcode_send_progress_min_interval_ms) {
            return;
        }

        if (total_exec <= k_max_gcode_stream_lines) {
            post_status(Glib::ustring::compose(_("Sending G-code: line %1 / %2..."),
                                               static_cast<guint64>(sent), static_cast<guint64>(total_exec)),
                        false);
        } else {
            post_status(
                Glib::ustring::compose(_("Sending G-code: line %1 (program exceeds %2-line limit; sending will stop on error)..."),
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
        post_status(_("No executable lines to send (only blanks or comments)."), false);
    } else if (send_from_cursor) {
        post_status(Glib::ustring::compose(_("Sent %1 lines of G-code (starting from editor line %2)."),
                                           static_cast<guint64>(sent), static_cast<guint64>(editor_line_1)),
                    false);
    } else {
        post_status(Glib::ustring::compose(_("Sent %1 lines of G-code."), static_cast<guint64>(sent)), false);
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
                _("The next layer is about to start drawing.\nIf you are plotting by layer or color, change the pen now and click Yes to continue."),
                true, Gtk::MessageType::QUESTION, Gtk::ButtonsType::YES_NO, true);
            dlg.set_secondary_text(
                _("This follows the AxiDraw-style manual pen-change flow: raise pen, optionally return home, confirm, then resume from the pause point."));
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

void GrblPanelSender::run_direct_send_worker(GrblPanelSenderContext const &context,
                                             std::unique_lock<std::mutex> &port_lock, SPDocument *doc,
                                             SPDesktop *desktop, Inkscape::Selection *selection,
                                             bool const use_current_layer_without_selection,
                                             Inkscape::Axidraw::GrblExportParams const &params, Gtk::Window *win)
{
    auto const finish = [&context, &port_lock] { context.finish_worker(port_lock); };

    auto *serial = context.link ? context.link->serial_port() : nullptr;
    if (!serial || !serial->is_open()) {
        context.post_status(_("Serial port is disconnected."), true);
        finish();
        return;
    }

    auto ctx = make_direct_send_context(desktop, selection, use_current_layer_without_selection, context.cancel,
                                        params.manual_pen_change && params.pen_change_prompt ? win : nullptr);
    attach_direct_send_progress_callbacks(context.post_status, ctx);

    context.with_plot_waits([&] {
        std::string err;
        std::size_t strokes = 0;
        Inkscape::Axidraw::GrblPlotStats stats{};
        if (!Inkscape::Axidraw::export_paths_to_grbl(*serial, doc, params, ctx, err, &strokes, &stats)) {
            context.post_gcode_stream_result(err);
            return;
        }

        context.refresh_plot_feedback_after_gcode_change();
        context.post_status(make_direct_send_done_status(strokes, stats), false);
    });
    finish();
}

void GrblPanelSender::run_editor_gcode_send_worker(GrblPanelSenderContext const &context,
                                                   std::unique_lock<std::mutex> &port_lock, std::string text,
                                                   std::size_t const total_exec, bool const send_from_cursor,
                                                   guint const editor_line_1)
{
    auto const finish = [&context, &port_lock] { context.finish_worker(port_lock); };

    auto *link = context.link;
    if (!(link && link->is_open())) {
        context.post_not_connected_status();
        finish();
        return;
    }

    if (send_from_cursor) {
        context.post_status(
            Glib::ustring::compose(_("Sending G-code starting from editor line %1..."),
                                   static_cast<guint64>(editor_line_1)),
            false);
    }

    GcodeSendProgressTracker progress{.post_status = context.post_status, .total_exec = total_exec};

    context.with_plot_waits([&] {
        std::string err;
        bool write_failed = false;
        std::size_t sent = 0;
        for_each_executable_gcode_line(text, [&](std::string const &line) {
            if (!err.empty() || write_failed) {
                return;
            }
            if (sent >= k_max_gcode_stream_lines) {
                err = _("Too many G-code lines (limit exceeded).");
                return;
            }
            if (!link->send_line_wait_ok(line, err)) {
                write_failed = true;
                return;
            }
            ++sent;
            progress.maybe_post(sent, false);
        });

        if (write_failed) {
            context.post_gcode_stream_result(err);
            return;
        }
        if (!err.empty()) {
            context.post_gcode_stream_result(err);
        } else {
            post_gcode_send_completion(context.post_status, send_from_cursor, editor_line_1, sent);
        }
    });
    finish();
}

} // namespace Inkscape::UI::Dialog
