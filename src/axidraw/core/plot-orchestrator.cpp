// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * GRBL plot orchestration (native AxiDraw migration).
 *
 * Plot parameters are loaded through `grbl_export_params_from_preferences()` so the streamed job
 * matches the GRBL control panel’s “Fill from drawing” preview and `grbl-export.h` semantics.
 *
 * For pen lift, set `/options/grbl/pen-control` to `z` (Z G-code lines) or `m3m5` (M5/M3 as on
 * Paixi `NullSpindle`). When `m3m5` is set, `pen-up-cmd` and `pen-down-cmd` are not used. Serial
 * defaults match common Grbl/ESP32. See `inkscape-axidraw/Grbl_Esp32` (e.g. `Config.h`,
 * `Spindles/NullSpindle.cpp`); this orchestrator does not send T/M6 by itself.
 */

#include "plot-orchestrator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/messagedialog.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/serial-port.h"
#include "axidraw/pipeline/grbl-export.h"
#include "desktop.h"
#include "document.h"
#include "preferences.h"
#include "ui/dialog-run.h"
#include "ui/pack.h"
#include "util/scope_exit.h"

namespace Inkscape::Axidraw {

namespace {

constexpr auto pref_device = "/options/grbl/serial-device";
constexpr auto pref_baud = "/options/grbl/baud";
constexpr auto pref_layer = "/options/grbl/limit-to-current-layer";
constexpr int k_serial_open_timeout_ms = 4000;

std::string describe_probe_failure(Glib::ustring const &device, int baud, GrblProbeResult const &probe)
{
    if (probe.response_line.empty()) {
        return Glib::ustring::compose(
                   _("No GRBL status response on %1 at %2 baud. Check the serial port, baud rate, and controller power."),
                   device, baud)
            .raw();
    }
    return Glib::ustring::compose(
               _("The selected serial port %1 (%2 baud) answered, but not like a GRBL controller:\n%3"), device, baud,
               Glib::ustring(probe.response_line))
        .raw();
}

std::string describe_open_failure(Glib::ustring const &device, int baud, bool timed_out)
{
    if (timed_out) {
        return Glib::ustring::compose(
                   _("Timed out while opening serial port %1 at %2 baud. Check whether another program is holding the "
                     "port, whether the USB/serial driver is responsive, and whether the controller is powered."),
                   device, baud)
            .raw();
    }
    return Glib::ustring::compose(_("Could not open serial port %1 at %2 baud."), device, baud).raw();
}

enum class ConnectWorkerStatus {
    Ok,
    OpenFailed,
    ProbeFailed,
    Cancelled,
};

struct ConnectWorkerResult {
    ConnectWorkerStatus status = ConnectWorkerStatus::OpenFailed;
    bool open_timed_out = false;
    GrblProbeResult probe;
    std::unique_ptr<SerialPort> port;
};

ConnectWorkerResult connect_and_probe_serial(std::string device, int baud, std::atomic<bool> const &cancel)
{
    ConnectWorkerResult result;
    if (cancel.load(std::memory_order_acquire)) {
        result.status = ConnectWorkerStatus::Cancelled;
        return result;
    }

    auto port = std::make_unique<SerialPort>();
    if (!port->open(device, baud, k_serial_open_timeout_ms)) {
        result.status = ConnectWorkerStatus::OpenFailed;
        result.open_timed_out = port->last_open_timed_out();
        return result;
    }

    if (cancel.load(std::memory_order_acquire)) {
        port->close();
        result.status = ConnectWorkerStatus::Cancelled;
        return result;
    }

    result.probe = probe_open_grbl(*port);
    if (cancel.load(std::memory_order_acquire)) {
        port->close();
        result.status = ConnectWorkerStatus::Cancelled;
        return result;
    }

    if (!result.probe.ok) {
        result.status = ConnectWorkerStatus::ProbeFailed;
        return result;
    }

    result.status = ConnectWorkerStatus::Ok;
    result.port = std::move(port);
    return result;
}

std::optional<Glib::ustring> prompt_serial_device(Gtk::Window &parent, Glib::ustring const &previous)
{
    Gtk::Dialog dlg(_("GRBL serial port"), true);
    dlg.set_transient_for(parent);
    dlg.set_modal(true);
    dlg.set_resizable(false);

    Gtk::Label explanation;
    explanation.set_wrap(true);
    explanation.set_markup(
        _("Enter the serial device path used by your GRBL controller.\n"
          "Examples: <tt>COM4</tt> on Windows, <tt>/dev/ttyUSB0</tt> on Linux."));

    Gtk::Entry entry;
    if (!previous.empty()) {
        entry.set_text(previous);
    } else {
#ifdef _WIN32
        entry.set_text("COM3");
#else
        entry.set_text("/dev/ttyUSB0");
#endif
    }

    auto *content = dlg.get_content_area();
    Inkscape::UI::pack_start(*content, explanation, false, false, 8);
    Inkscape::UI::pack_start(*content, entry, false, false, 8);

    dlg.add_button(_("_Cancel"), Gtk::ResponseType::CANCEL);
    dlg.add_button(_("_OK"), Gtk::ResponseType::OK);
    dlg.set_default_response(Gtk::ResponseType::OK);

    int const status = Inkscape::UI::dialog_run(dlg);
    if (status != static_cast<int>(Gtk::ResponseType::OK)) {
        return std::nullopt;
    }

    return entry.get_text();
}

} // namespace

GrblConnectAttempt PlotOrchestrator::run_plot_grbl(SPDocument *doc, SPDesktop *desktop, Gtk::Window &parent,
                                                   std::string &message_out)
{
    message_out.clear();
    if (!desktop) {
        message_out = _("No active desktop for GRBL output.");
        return GrblConnectAttempt::Failed;
    }

    auto *prefs = Inkscape::Preferences::get();
    Glib::ustring device = prefs->getString(pref_device);
    int const baud = prefs->getIntLimited(pref_baud, 115200, 9600, 230400);

    if (device.empty()) {
        auto chosen = prompt_serial_device(parent, device);
        if (!chosen || chosen->empty()) {
            return GrblConnectAttempt::Cancelled;
        }
        device = *chosen;
        prefs->setString(pref_device, device);
        prefs->save();
    }

    std::atomic<bool> connect_cancel{false};

    Gtk::Dialog connect_dlg(_("Connecting to GRBL plotter"), true);
    connect_dlg.set_transient_for(parent);
    connect_dlg.set_modal(true);
    connect_dlg.set_resizable(false);
    connect_dlg.add_button(_("_Cancel"), Gtk::ResponseType::CANCEL);

    Gtk::Label connect_label;
    connect_label.set_wrap(true);
    connect_label.set_margin_start(8);
    connect_label.set_margin_end(8);
    connect_label.set_margin_top(8);
    connect_label.set_margin_bottom(8);
    connect_label.set_markup(
        Glib::ustring::compose(_("Opening %1 at %2 baud and probing for a GRBL response..."), device, baud));
    if (auto *content = connect_dlg.get_content_area()) {
        Inkscape::UI::pack_start(*content, connect_label, false, false, 0);
    }
    connect_dlg.signal_response().connect([&](int response) {
        if (response == static_cast<int>(Gtk::ResponseType::CANCEL)) {
            connect_cancel.store(true, std::memory_order_release);
            connect_label.set_markup(
                _("Cancelling connection attempt... waiting for the current serial operation to finish."));
        }
    });
    connect_dlg.show();

    auto connect_result = std::make_shared<ConnectWorkerResult>();
    std::atomic<bool> connect_done{false};
    std::thread connect_worker([connect_result, device_raw = device.raw(), baud, &connect_cancel, &connect_done] {
        *connect_result = connect_and_probe_serial(device_raw, baud, connect_cancel);
        connect_done.store(true, std::memory_order_release);
    });

    auto const ctx = Glib::MainContext::get_default();
    while (!connect_done.load(std::memory_order_acquire)) {
        if (ctx) {
            while (ctx->iteration(false)) {
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    connect_worker.join();
    connect_dlg.close();

    if (connect_result->status == ConnectWorkerStatus::Cancelled) {
        message_out = _("Plot cancelled.");
        return GrblConnectAttempt::Cancelled;
    }
    if (connect_result->status == ConnectWorkerStatus::OpenFailed) {
        message_out = describe_open_failure(device, baud, connect_result->open_timed_out);
        return GrblConnectAttempt::Failed;
    }
    if (connect_result->status == ConnectWorkerStatus::ProbeFailed) {
        message_out = describe_probe_failure(device, baud, connect_result->probe);
        return GrblConnectAttempt::Failed;
    }

    auto port = std::move(connect_result->port);
    auto const probe = connect_result->probe;
    if (!port || !port->is_open()) {
        message_out = describe_open_failure(device, baud, false);
        return GrblConnectAttempt::Failed;
    }

    port->purge_io();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::string junk;
        for (int i = 0; i < 20; ++i) {
            if (!port->read_line(junk, 60)) {
                break;
            }
        }
    }

    std::atomic<bool> cancel{false};

    auto pump = []() {
        if (auto const ctx = Glib::MainContext::get_default()) {
            while (ctx->iteration(false)) {
            }
        }
    };

    Gtk::Dialog wait_dlg(_("Sending to plotter…"), true);
    wait_dlg.set_transient_for(parent);
    wait_dlg.set_modal(true);
    wait_dlg.add_button(_("_Cancel"), Gtk::ResponseType::CANCEL);

    Gtk::Label wlab;
    wlab.set_wrap(true);
    wlab.set_markup(_(
        "Streaming G-code to the machine. <b>Cancel</b> stops the job after the current line finishes sending."));
    wlab.set_margin_start(8);
    wlab.set_margin_end(8);
    wlab.set_margin_top(8);
    wlab.set_margin_bottom(8);
    if (auto *wcontent = wait_dlg.get_content_area()) {
        Inkscape::UI::pack_start(*wcontent, wlab, false, false, 0);
    }
    wait_dlg.signal_response().connect([&](int r) {
        if (r == static_cast<int>(Gtk::ResponseType::CANCEL)) {
            cancel.store(true, std::memory_order_relaxed);
        }
    });
    wait_dlg.show();

    grbl_begin_plot_waits(pump, &cancel);
    scope_exit const close_wait_dlg([&wait_dlg] { wait_dlg.close(); });
    scope_exit const end_plot_waits([] { grbl_end_plot_waits(); });

    if (cancel.load(std::memory_order_relaxed)) {
        message_out = _("Plot cancelled.");
        return GrblConnectAttempt::Cancelled;
    }
    std::string const status = probe.response_line;

    GrblExportParams params;
    grbl_export_params_from_preferences(prefs, params);

    GrblExportContext exctx;
    exctx.desktop = desktop;
    exctx.selection = desktop->getSelection();
    exctx.use_current_layer_without_selection = prefs->getBool(pref_layer, false);
    exctx.cancel = &cancel;
    if (params.manual_pen_change && params.pen_change_prompt) {
        exctx.on_manual_pen_change_between_layers =
            [params, &parent](double resume_x_mm, double resume_y_mm) -> bool {
                (void)resume_x_mm;
                (void)resume_y_mm;
                Gtk::MessageDialog dlg(
                    parent,
                    _("The next Inkscape layer is about to plot.\nIf you use one layer per pen or colour, change pens "
                      "now, then click Yes to continue."),
                    true, Gtk::MessageType::QUESTION, Gtk::ButtonsType::YES_NO, true);
                dlg.set_secondary_text(_(
                    "Same pattern as inkscape-axidraw: pen up, optional rapid to work XY zero, then rapid back to the "
                    "pause position."));
                return Inkscape::UI::dialog_run(dlg) == Gtk::ResponseType::YES;
            };
    }

    struct PlotWaitProgress {
        std::size_t stroke_done = 0;
        std::size_t stroke_total = 0;
        std::size_t lines_sent = 0;
        std::size_t lines_est = 0;
    };
    auto const prog = std::make_shared<PlotWaitProgress>();
    using clock = std::chrono::steady_clock;
    auto const last_paint = std::make_shared<clock::time_point>(clock::time_point::min());

    auto const refresh_plot_wait_label = [&wlab, prog, last_paint]() {
        if (prog->lines_est == 0 && prog->stroke_total == 0) {
            return;
        }
        clock::time_point const now = clock::now();
        constexpr auto k_min_interval = std::chrono::milliseconds(75);
        bool const at_end = (prog->lines_est > 0 && prog->lines_sent >= prog->lines_est) ||
                            (prog->stroke_total > 0 && prog->stroke_done >= prog->stroke_total);
        if (!at_end && now - *last_paint < k_min_interval) {
            return;
        }
        *last_paint = now;

        if (prog->lines_est > 0 && prog->lines_sent > 0 && prog->stroke_total > 0 && prog->stroke_done > 0) {
            wlab.set_markup(Glib::ustring::compose(
                _("Streaming G-code to the machine (%1 of %2 strokes, line %3 of ~%4). <b>Cancel</b> stops the job after "
                  "the current line finishes sending."),
                static_cast<guint64>(prog->stroke_done), static_cast<guint64>(prog->stroke_total),
                static_cast<guint64>(prog->lines_sent), static_cast<guint64>(prog->lines_est)));
        } else if (prog->lines_est > 0 && prog->lines_sent > 0) {
            wlab.set_markup(Glib::ustring::compose(
                _("Streaming G-code to the machine (line %1 of ~%2). <b>Cancel</b> stops the job after the current line "
                  "finishes sending."),
                static_cast<guint64>(prog->lines_sent), static_cast<guint64>(prog->lines_est)));
        } else if (prog->stroke_total > 0 && prog->stroke_done > 0) {
            wlab.set_markup(Glib::ustring::compose(
                _("Streaming G-code to the machine (%1 of %2 strokes). <b>Cancel</b> stops the job after the current "
                  "line finishes sending."),
                static_cast<guint64>(prog->stroke_done), static_cast<guint64>(prog->stroke_total)));
        }
    };

    exctx.on_plot_stroke_progress = [prog, refresh_plot_wait_label](std::size_t done, std::size_t total) {
        if (total == 0) {
            return;
        }
        prog->stroke_done = done;
        prog->stroke_total = total;
        refresh_plot_wait_label();
    };

    exctx.on_plot_gcode_line_progress = [prog, refresh_plot_wait_label](std::size_t sent, std::size_t est) {
        prog->lines_sent = sent;
        prog->lines_est = est;
        refresh_plot_wait_label();
    };

    std::size_t nstrokes = 0;
    GrblPlotStats stats{};
    std::string err;
    if (!export_paths_to_grbl(*port, doc, params, exctx, err, &nstrokes, &stats)) {
        if (err == grbl_error_user_cancelled()) {
            message_out = _("Plot cancelled.");
            return GrblConnectAttempt::Cancelled;
        }
        message_out = grbl_error_to_user_message(err);
        return GrblConnectAttempt::Failed;
    }

    Glib::ustring summary = Glib::ustring::compose(
        _("Plot finished: %1 stroke(s) sent.\nController status before plot:\n%2"), static_cast<guint64>(nstrokes),
        Glib::ustring(status));
    if (stats.has_bounds_mm) {
        std::ostringstream wxh;
        wxh << std::fixed << std::setprecision(1) << (stats.max_x_mm - stats.min_x_mm) << " × "
             << (stats.max_y_mm - stats.min_y_mm);
        summary += "\n";
        summary += Glib::ustring::compose(_("Approx. work area (machine mm, after preferences): %1 mm."),
                                          Glib::ustring(wxh.str()));
    }
    if (stats.has_length_stats && (stats.draw_length_mm + stats.travel_length_mm) > 1e-9) {
        double const total = stats.draw_length_mm + stats.travel_length_mm;
        double const air = (stats.travel_length_mm / total) * 100.0;
        std::ostringstream lengths;
        lengths << std::fixed << std::setprecision(1) << stats.draw_length_mm << " / " << stats.travel_length_mm;
        std::ostringstream ratio;
        ratio << std::fixed << std::setprecision(1) << air;
        summary += "\n";
        summary += Glib::ustring::compose(_("Draw/travel length: %1 mm. Air-run ratio: %2%%."),
                                          Glib::ustring(lengths.str()), Glib::ustring(ratio.str()));
    }
    message_out.assign(summary.c_str());
    return GrblConnectAttempt::Ok;
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
