// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-sender.h"

#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/ustring.h>
#include <gtkmm/messagedialog.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/grbl-link.h"
#include "axidraw/device/serial-port.h"
#include "axidraw/pipeline/grbl-export.h"
#include "ui/dialog/grbl-editor-gcode.h"
#include "ui/dialog-run.h"

namespace Inkscape::UI::Dialog {
namespace {

constexpr int k_gcode_send_progress_min_interval_ms = 350;
constexpr std::size_t k_gcode_send_progress_line_stride = 80;
constexpr std::size_t k_max_gcode_stream_lines = 200000;
constexpr auto k_motor_disable_gcode = "MD";

void prepare_stream_link(Inkscape::Axidraw::GrblLink *link)
{
    if (!(link && link->is_open())) {
        return;
    }

    // Clear any delayed replies from connect/probe/firmware-sync/status-poll work
    // so the first streamed G-code line cannot consume a stale "ok".
    link->purge_io();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string junk;
    for (int i = 0; i < 20; ++i) {
        if (!link->read_line(junk, 60)) {
            break;
        }
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

bool send_motor_disable_if_possible(Inkscape::Axidraw::GrblLink *link, std::string &err_out)
{
    if (!(link && link->is_open())) {
        err_out = _("Serial port is disconnected.");
        return false;
    }
    return link->send_line_wait_ok(k_motor_disable_gcode, err_out);
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
        ctx.on_manual_pen_change_between_layers = [win](double const resume_x_mm, double const resume_y_mm) -> bool {
            std::mutex mutex;
            std::condition_variable cv;
            std::optional<bool> accepted;

            Glib::signal_idle().connect_once([win, resume_x_mm, resume_y_mm, &mutex, &cv, &accepted] {
                Gtk::MessageDialog dlg(
                    *win,
                    _("下一图层即将开始绘制。\n请现在换笔，然后点击“是”继续。"),
                    false, Gtk::MessageType::QUESTION, Gtk::ButtonsType::YES_NO, true);
                dlg.set_title(_("手动换笔"));
                dlg.set_secondary_text(
                    Glib::ustring::compose(
                        _("流程：先抬笔；如果启用了“先回原点换笔”，机器会先回到 X0 Y0；确认后再回到断点继续。\n继续位置：X=%1 mm, Y=%2 mm"),
                        Glib::ustring::format(std::fixed, std::setprecision(3), resume_x_mm),
                        Glib::ustring::format(std::fixed, std::setprecision(3), resume_y_mm)));
                auto const response = Inkscape::UI::dialog_run(dlg);
                {
                    std::lock_guard const lock(mutex);
                    accepted = (response == Gtk::ResponseType::YES);
                }
                cv.notify_one();
            });

            std::unique_lock lock(mutex);
            cv.wait(lock, [&accepted] { return accepted.has_value(); });
            return *accepted;
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

    prepare_stream_link(context.link);

    auto ctx = make_direct_send_context(desktop, selection, use_current_layer_without_selection, context.cancel,
                                        params.manual_pen_change && params.pen_change_prompt ? win : nullptr);
    attach_direct_send_progress_callbacks(context.post_status, ctx);

    context.with_plot_waits([&] {
        std::string err;
        std::size_t strokes = 0;
        Inkscape::Axidraw::GrblPlotStats stats{};
        if (!Inkscape::Axidraw::export_paths_to_grbl(*serial, doc, params, ctx, err, &strokes, &stats)) {
            if (err != Inkscape::Axidraw::grbl_error_user_cancelled() &&
                !(context.should_defer_motor_disable_cleanup && context.should_defer_motor_disable_cleanup())) {
                std::string cleanup_err;
                send_motor_disable_if_possible(context.link, cleanup_err);
            }
            context.post_gcode_stream_result(err);
            return;
        }

        std::string cleanup_err;
        if (!send_motor_disable_if_possible(context.link, cleanup_err)) {
            context.post_gcode_stream_result(cleanup_err);
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

    prepare_stream_link(link);

    GcodeSendProgressTracker progress{.post_status = context.post_status, .total_exec = total_exec};

    context.with_plot_waits([&] {
        std::string err;
        bool write_failed = false;
        std::size_t sent = 0;
        for_each_executable_grbl_gcode_line(text, [&](std::string const &line) {
            if (!err.empty() || write_failed) {
                return false;
            }
            if (sent >= k_max_gcode_stream_lines) {
                err = _("Too many G-code lines (limit exceeded).");
                return false;
            }
            if (!link->send_line_wait_ok(line, err)) {
                write_failed = true;
                return false;
            }
            ++sent;
            progress.maybe_post(sent, false);
            return true;
        });

        if (write_failed) {
            if (err != Inkscape::Axidraw::grbl_error_user_cancelled() &&
                !(context.should_defer_motor_disable_cleanup && context.should_defer_motor_disable_cleanup())) {
                std::string cleanup_err;
                send_motor_disable_if_possible(link, cleanup_err);
            }
            context.post_gcode_stream_result(err);
            return;
        }
        if (!err.empty()) {
            if (err != Inkscape::Axidraw::grbl_error_user_cancelled() &&
                !(context.should_defer_motor_disable_cleanup && context.should_defer_motor_disable_cleanup())) {
                std::string cleanup_err;
                send_motor_disable_if_possible(link, cleanup_err);
            }
            context.post_gcode_stream_result(err);
        } else {
            std::string cleanup_err;
            if (!send_motor_disable_if_possible(link, cleanup_err)) {
                context.post_gcode_stream_result(cleanup_err);
                return;
            }
            post_gcode_send_completion(context.post_status, send_from_cursor, editor_line_1, sent);
        }
    });
    finish();
}

} // namespace Inkscape::UI::Dialog
