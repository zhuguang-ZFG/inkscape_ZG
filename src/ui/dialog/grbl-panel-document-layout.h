// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Pure document-to-bed layout planning helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_DOCUMENT_LAYOUT_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_DOCUMENT_LAYOUT_H

#include <glibmm/ustring.h>

namespace Geom {
class Rect;
}

namespace Inkscape::UI::Dialog {

struct GrblFitToBedPlan
{
    bool valid = false;
    bool shrink_applied = false;
    double scale = 1.0;
    double translate_x = 0.0;
    double translate_y = 0.0;
};

struct GrblCenterToBedPlan
{
    bool valid = false;
    double translate_x = 0.0;
    double translate_y = 0.0;
};

GrblFitToBedPlan make_grbl_fit_to_bed_plan(Geom::Rect const &bounds, double bed_w_doc, double bed_h_doc);
GrblCenterToBedPlan make_grbl_center_to_bed_plan(Geom::Rect const &bounds, double bed_w_doc, double bed_h_doc);
Glib::ustring build_grbl_fit_to_bed_status(double scale, bool shrink_applied);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_DOCUMENT_LAYOUT_H
