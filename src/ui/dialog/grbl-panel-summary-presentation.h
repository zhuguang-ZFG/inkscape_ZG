// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared job/scale summary presentation helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_SUMMARY_PRESENTATION_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_SUMMARY_PRESENTATION_H

#include <cstddef>

#include <glibmm/ustring.h>

#include "axidraw/pipeline/grbl-export.h"

namespace Inkscape::UI::Dialog {

Glib::ustring build_grbl_layout_scale_summary_markup(Inkscape::Axidraw::GrblPlotStats const &stats,
                                                     double bed_width_mm, double bed_height_mm);
Glib::ustring build_grbl_job_summary_markup(Inkscape::Axidraw::GrblPlotStats const &stats, double bed_width_mm,
                                            double bed_height_mm);
Glib::ustring build_grbl_fill_gcode_status(std::size_t strokes, Inkscape::Axidraw::GrblPlotStats const &stats);
Glib::ustring build_grbl_no_active_document_job_summary_markup();
Glib::ustring build_grbl_no_active_document_scale_summary_markup();
Glib::ustring build_grbl_analysis_error_job_summary_markup(std::string const &err);
Glib::ustring build_grbl_analysis_error_scale_summary_markup(std::string const &err);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_SUMMARY_PRESENTATION_H
