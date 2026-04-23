// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Application actions for native GRBL / AxiDraw integration.
 */

#include "actions-axidraw.h"

#include <giomm/liststore.h>
#include <glibmm/fileutils.h>
#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/messagedialog.h>

#include <iomanip>
#include <sstream>

#include <sigc++/adaptors/bind.h>

#include "actions/actions-extra-data.h"
#include "axidraw/core/plot-orchestrator.h"
#include "axidraw/pipeline/grbl-export.h"
#include "desktop.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "io/sys.h"
#include "preferences.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog-run.h"
#include "ui/interface.h"

using Inkscape::choose_file_save;

namespace {

const Glib::ustring SECTION = NC_("Action Section", "GRBL Plotter");

void axidraw_plot(InkscapeApplication *app)
{
    if (!app->gtk_app()) {
        return;
    }

    auto *doc = app->get_active_document();
    auto *desktop = app->get_active_desktop();
    auto *win = app->get_active_window();
    if (!doc || !desktop || !win) {
        sp_ui_error_dialog(_("No active document or window."));
        return;
    }

    std::string detail;
    auto const attempt = Inkscape::Axidraw::PlotOrchestrator::run_plot_grbl(doc, desktop, *win, detail);

    switch (attempt) {
    case Inkscape::Axidraw::GrblConnectAttempt::Cancelled:
        return;
    case Inkscape::Axidraw::GrblConnectAttempt::Failed: {
        Glib::ustring msg = !detail.empty() ? Glib::ustring(detail)
                                            : _("GRBL handshake failed for an unknown reason.");
        sp_ui_error_dialog(msg.c_str());
        return;
    }
    case Inkscape::Axidraw::GrblConnectAttempt::Ok: {
        Gtk::MessageDialog dlg(*win, Glib::ustring(detail), false, Gtk::MessageType::INFO,
                               Gtk::ButtonsType::OK);
        Inkscape::UI::dialog_run(dlg);
        return;
    }
    }
}

constexpr auto k_pref_grbl_layer = "/options/grbl/limit-to-current-layer";
constexpr auto k_pref_save_grbl_gcode_dir = "/dialogs/grblcontrol/save_gcode_dir";
constexpr std::size_t k_max_grbl_gcode_file_bytes = 32u * 1024u * 1024u;

void axidraw_export_gcode_to_file(InkscapeApplication *app)
{
    if (!app->gtk_app()) {
        return;
    }
    auto *doc = app->get_active_document();
    auto *desktop = app->get_active_desktop();
    auto *win = app->get_active_window();
    if (!doc || !desktop || !win) {
        sp_ui_error_dialog(_("No active document or window."));
        return;
    }

    auto *prefs = Inkscape::Preferences::get();
    Inkscape::Axidraw::GrblExportParams params;
    Inkscape::Axidraw::grbl_export_params_from_preferences(prefs, params);

    Inkscape::Axidraw::GrblExportContext ctx;
    ctx.desktop = desktop;
    ctx.selection = desktop->getSelection();
    ctx.use_current_layer_without_selection = prefs->getBool(k_pref_grbl_layer, false);
    ctx.cancel = nullptr;

    std::string folder;
    Inkscape::UI::Dialog::get_start_directory(folder, k_pref_save_grbl_gcode_dir, true);

    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto gcf = Gtk::FileFilter::create();
    gcf->set_name(_("G-code"));
    gcf->add_suffix("nc");
    gcf->add_suffix("gcode");
    gcf->add_suffix("tap");
    gcf->add_suffix("cnc");
    gcf->add_suffix("txt");
    filters->append(gcf);
    auto all = Gtk::FileFilter::create();
    all->set_name(_("All files"));
    all->add_pattern("*");
    filters->append(all);

    std::string initial = "plot.nc";
    if (char const *fn = doc->getDocumentFilename()) {
        std::string base = Glib::path_get_basename(fn);
        Inkscape::IO::remove_file_extension(base);
        if (!base.empty()) {
            initial = std::move(base) + ".nc";
        }
    }

    Glib::RefPtr<Gio::File> const dest =
        choose_file_save(_("Export GRBL G-code"), win, filters, initial, folder);
    if (!dest) {
        return;
    }
    std::string const path = dest->get_path();
    if (path.empty()) {
        sp_ui_error_dialog(_("Could not determine a local file path to save."));
        return;
    }

    std::string gcode;
    std::string err;
    Inkscape::Axidraw::GrblPlotStats stats{};
    if (!Inkscape::Axidraw::build_grbl_plot_gcode_string(doc, params, ctx, gcode, err, nullptr,
                                                         k_max_grbl_gcode_file_bytes, &stats)) {
        sp_ui_error_dialog(err.empty() ? _("Could not build G-code from the document.") : err.c_str());
        return;
    }
    try {
        Glib::file_set_contents(path, gcode);
    } catch (Glib::FileError const &e) {
        sp_ui_error_dialog(e.what());
        return;
    }
    prefs->setString(k_pref_save_grbl_gcode_dir, folder);

    Glib::ustring const primary =
        Glib::ustring::compose(_("GRBL G-code saved to:\n%1"), dest->get_parse_name());
    Gtk::MessageDialog dlg(*win, primary, false, Gtk::MessageType::INFO, Gtk::ButtonsType::OK);
    Glib::ustring secondary;
    if (stats.has_bounds_mm) {
        std::ostringstream wxh;
        wxh << std::fixed << std::setprecision(1) << (stats.max_x_mm - stats.min_x_mm) << " × "
             << (stats.max_y_mm - stats.min_y_mm);
        secondary = Glib::ustring::compose(_("Strokes: %1. Approx. work area (machine mm, after preferences): %2."),
                                           static_cast<guint64>(stats.stroke_count), Glib::ustring(wxh.str()));
    } else {
        secondary = Glib::ustring::compose(_("Strokes: %1."), static_cast<guint64>(stats.stroke_count));
    }
    if (stats.has_length_stats && (stats.draw_length_mm + stats.travel_length_mm) > 1e-9) {
        double const total = stats.draw_length_mm + stats.travel_length_mm;
        double const air = (stats.travel_length_mm / total) * 100.0;
        std::ostringstream lengths;
        lengths << std::fixed << std::setprecision(1) << stats.draw_length_mm << " / " << stats.travel_length_mm;
        std::ostringstream ratio;
        ratio << std::fixed << std::setprecision(1) << air;
        secondary += "\n";
        secondary += Glib::ustring::compose(_("Draw/travel length: %1 mm. Air-run ratio: %2%%."),
                                            Glib::ustring(lengths.str()), Glib::ustring(ratio.str()));
    }
    dlg.set_secondary_text(secondary);
    Inkscape::UI::dialog_run(dlg);
}

std::vector<std::vector<Glib::ustring>> const raw_data_axidraw = {
    // clang-format off
    {"app.axidraw-plot", N_("Send document to GRBL plotter…"), SECTION,
     N_("Send visible vector paths from the document to a GRBL controller over serial (G21/G90, pen up/down, G0/G1).")},
    {"app.axidraw-export-gcode", N_("Export GRBL G-code to file…"), SECTION,
     N_("Build the same G-code as “Send document to GRBL plotter…” and save it to disk without opening the serial port.")},
    // clang-format on
};

} // namespace

void add_actions_axidraw(InkscapeApplication *app)
{
    if (!app->gtk_app()) {
        return;
    }

    auto *gapp = app->gio_app();
    gapp->add_action("axidraw-plot", sigc::bind(sigc::ptr_fun(&axidraw_plot), app));
    gapp->add_action("axidraw-export-gcode", sigc::bind(sigc::ptr_fun(&axidraw_export_gcode_to_file), app));

    app->get_action_extra_data().add_data(raw_data_axidraw);
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
