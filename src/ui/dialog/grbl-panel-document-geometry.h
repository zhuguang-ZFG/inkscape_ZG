// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared document geometry helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_DOCUMENT_GEOMETRY_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_DOCUMENT_GEOMETRY_H

#include <glibmm/ustring.h>

class SPDocument;
class SPPage;

namespace Geom {
class Rect;
}

namespace Inkscape::UI::Dialog {

struct GrblPageRestoreState
{
    bool has_saved_page_restore = false;
    double saved_doc_width_px = 0.0;
    double saved_doc_height_px = 0.0;
    double saved_page_width_px = 0.0;
    double saved_page_height_px = 0.0;
};

struct GrblPageSyncToBedResult
{
    bool page_synced = false;
    bool unit_synced = false;
};

enum class GrblDocumentGeometryChange {
    sync_page_to_bed,
    fit_to_bed,
    center_to_bed,
    restore_page,
};

SPPage *get_grbl_target_page(SPDocument *doc);
bool grbl_page_restore_state_is_saved(GrblPageRestoreState const &state);
void capture_grbl_page_restore_state(SPDocument *doc, SPPage *page, GrblPageRestoreState &state);
void clear_grbl_page_restore_state(GrblPageRestoreState &state);
void apply_grbl_document_and_page_size_px(SPDocument *doc, SPPage *page, double doc_width_px, double doc_height_px,
                                          double page_width_px, double page_height_px);
bool sync_grbl_document_page_to_bed_mm(SPDocument *doc, SPPage *page, double width_mm, double height_mm,
                                       bool &unit_synced_out, GrblPageRestoreState &restore_state);
GrblPageSyncToBedResult apply_grbl_sync_page_to_bed_action(SPDocument *doc, double width_mm, double height_mm,
                                                           GrblPageRestoreState &restore_state);
bool prepare_grbl_document_bed_action(SPDocument *doc, double bed_width_mm, double bed_height_mm, Geom::Rect &bounds,
                                      double &bed_w_doc, double &bed_h_doc, Glib::ustring &error,
                                      Glib::ustring const &empty_message);
void finalize_grbl_document_geometry_change(SPDocument *doc, GrblDocumentGeometryChange change);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_DOCUMENT_GEOMETRY_H
