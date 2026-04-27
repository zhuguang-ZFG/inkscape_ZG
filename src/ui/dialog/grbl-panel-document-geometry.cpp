// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-document-geometry.h"

#include <glibmm/i18n.h>

#include "document.h"
#include "document-undo.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "page-manager.h"
#include "util/units.h"
#include "xml/node.h"

namespace {

constexpr double k_mm_per_in = 25.4;
constexpr double k_px_per_in = 96.0;
constexpr double k_mm_per_px = k_mm_per_in / k_px_per_in;

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

bool get_bed_size_in_document_units(SPDocument *doc, double const bed_width_mm, double const bed_height_mm,
                                    double &bed_w_doc, double &bed_h_doc)
{
    if (!doc) {
        return false;
    }

    auto const viewbox = doc->getViewBox();
    auto const page_px = doc->getDimensions();
    double const page_w_mm = page_px[Geom::X] * k_mm_per_px;
    double const page_h_mm = page_px[Geom::Y] * k_mm_per_px;

    if (viewbox.width() > 1e-9 && viewbox.height() > 1e-9 &&
        page_w_mm > 1e-9 && page_h_mm > 1e-9) {
        bed_w_doc = bed_width_mm * (viewbox.width() / page_w_mm);
        bed_h_doc = bed_height_mm * (viewbox.height() / page_h_mm);
    } else {
        bed_w_doc = bed_width_mm / k_mm_per_px;
        bed_h_doc = bed_height_mm / k_mm_per_px;
    }
    return bed_w_doc > 1e-9 && bed_h_doc > 1e-9;
}

} // namespace

namespace Inkscape::UI::Dialog {

SPPage *get_grbl_target_page(SPDocument *doc)
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

bool grbl_page_restore_state_is_saved(GrblPageRestoreState const &state)
{
    return state.has_saved_page_restore;
}

void capture_grbl_page_restore_state(SPDocument *doc, SPPage *page, GrblPageRestoreState &state)
{
    if (!doc || state.has_saved_page_restore) {
        return;
    }

    state.saved_doc_width_px = doc->getWidth().value("px");
    state.saved_doc_height_px = doc->getHeight().value("px");
    if (page) {
        auto rect = page->getRect();
        state.saved_page_width_px = rect.width();
        state.saved_page_height_px = rect.height();
    } else {
        state.saved_page_width_px = state.saved_doc_width_px;
        state.saved_page_height_px = state.saved_doc_height_px;
    }
    state.has_saved_page_restore = true;
}

void clear_grbl_page_restore_state(GrblPageRestoreState &state)
{
    state = {};
}

void apply_grbl_document_and_page_size_px(SPDocument *doc, SPPage *page, double const doc_width_px,
                                          double const doc_height_px, double const page_width_px,
                                          double const page_height_px)
{
    if (!doc) {
        return;
    }

    doc->setWidthAndHeight(Inkscape::Util::Quantity(doc_width_px, "px"),
                           Inkscape::Util::Quantity(doc_height_px, "px"), true);
    if (page) {
        auto rect = page->getRect();
        page->setRect(Geom::Rect::from_xywh(rect.min(), Geom::Point(page_width_px, page_height_px)));
    }
    doc->ensureUpToDate();
}

bool sync_grbl_document_page_to_bed_mm(SPDocument *doc, SPPage *page, double const width_mm, double const height_mm,
                                       bool &unit_synced_out, GrblPageRestoreState &restore_state)
{
    unit_synced_out = false;
    if (!doc || !(width_mm > 0.0) || !(height_mm > 0.0)) {
        return false;
    }

    capture_grbl_page_restore_state(doc, page, restore_state);
    Inkscape::Util::Quantity const width(width_mm, "mm");
    Inkscape::Util::Quantity const height(height_mm, "mm");
    apply_grbl_document_and_page_size_px(doc, page, width.value("px"), height.value("px"),
                                         width.value("px"), height.value("px"));

    if (auto action = doc->getActionGroup()->lookup_action("set-display-unit")) {
        action->activate(Glib::Variant<Glib::ustring>::create("mm"));
        unit_synced_out = true;
    }
    return true;
}

GrblPageSyncToBedResult apply_grbl_sync_page_to_bed_action(SPDocument *doc, double const width_mm,
                                                           double const height_mm,
                                                           GrblPageRestoreState &restore_state)
{
    GrblPageSyncToBedResult result;
    if (!doc) {
        return result;
    }

    result.page_synced = sync_grbl_document_page_to_bed_mm(
        doc, get_grbl_target_page(doc), width_mm, height_mm, result.unit_synced, restore_state);
    if (result.page_synced) {
        finalize_grbl_document_geometry_change(doc, GrblDocumentGeometryChange::sync_page_to_bed);
    }
    return result;
}

bool prepare_grbl_document_bed_action(SPDocument *doc, double const bed_width_mm, double const bed_height_mm,
                                      Geom::Rect &bounds, double &bed_w_doc, double &bed_h_doc,
                                      Glib::ustring &error, Glib::ustring const &empty_message)
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

void finalize_grbl_document_geometry_change(SPDocument *doc, GrblDocumentGeometryChange const change)
{
    if (!doc) {
        return;
    }
    doc->setModifiedSinceSave();
    switch (change) {
        case GrblDocumentGeometryChange::sync_page_to_bed:
            Inkscape::DocumentUndo::done(doc, RC_("Undo", "按机器行程同步页面尺寸和单位"), "");
            break;
        case GrblDocumentGeometryChange::fit_to_bed:
            Inkscape::DocumentUndo::done(doc, RC_("Undo", "按机器行程缩放图稿"), "");
            break;
        case GrblDocumentGeometryChange::center_to_bed:
            Inkscape::DocumentUndo::done(doc, RC_("Undo", "按机器行程居中图稿"), "");
            break;
        case GrblDocumentGeometryChange::restore_page:
            Inkscape::DocumentUndo::done(doc, RC_("Undo", "恢复原页面尺寸"), "");
            break;
    }
}

} // namespace Inkscape::UI::Dialog
