// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-document-layout.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include <2geom/rect.h>
#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {

GrblFitToBedPlan make_grbl_fit_to_bed_plan(Geom::Rect const &bounds, double const bed_w_doc, double const bed_h_doc)
{
    GrblFitToBedPlan plan;
    double const content_w = bounds.width();
    double const content_h = bounds.height();
    if (!(content_w > 0.0) || !(content_h > 0.0) || !(bed_w_doc > 0.0) || !(bed_h_doc > 0.0)) {
        return plan;
    }

    auto scale = std::min(bed_w_doc / content_w, bed_h_doc / content_h);
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        return plan;
    }
    if (scale > 1.0) {
        scale = 1.0;
    }

    plan.valid = true;
    plan.scale = scale;
    plan.shrink_applied = scale < 0.999999;
    plan.translate_x = -bounds.min()[Geom::X];
    plan.translate_y = -bounds.min()[Geom::Y];
    return plan;
}

GrblCenterToBedPlan make_grbl_center_to_bed_plan(Geom::Rect const &bounds, double const bed_w_doc,
                                                 double const bed_h_doc)
{
    GrblCenterToBedPlan plan;
    double const content_w = bounds.width();
    double const content_h = bounds.height();
    if (!(content_w > 0.0) || !(content_h > 0.0) || !(bed_w_doc > 0.0) || !(bed_h_doc > 0.0)) {
        return plan;
    }

    plan.valid = true;
    plan.translate_x = ((bed_w_doc - content_w) * 0.5) - bounds.min()[Geom::X];
    plan.translate_y = ((bed_h_doc - content_h) * 0.5) - bounds.min()[Geom::Y];
    return plan;
}

Glib::ustring build_grbl_fit_to_bed_status(double const scale, bool const shrink_applied)
{
    if (!shrink_applied) {
        return _("图稿本身已小于机器行程，已仅将其移动到机器原点范围内。");
    }

    std::ostringstream msg;
    msg << _("已将图稿等比缩小并移动到机器行程内。缩放比例 ");
    msg << std::fixed << std::setprecision(1) << (scale * 100.0) << "%。";
    return Glib::ustring(msg.str());
}

} // namespace Inkscape::UI::Dialog
