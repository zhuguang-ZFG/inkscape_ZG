// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-feedback-presentation.h"

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {
namespace {

Glib::ustring make_preview_summary(bool const machine_space, std::size_t const included, std::size_t const total,
                                   bool const clip_approx)
{
    if (!machine_space) {
        return Glib::ustring::compose(
            _("文档空间预览仅显示了 %1 / %2 段路径；如果图稿过于复杂，请简化图稿或关闭该预览。"),
            static_cast<guint64>(included), static_cast<guint64>(total));
    }
    if (clip_approx && total > included) {
        return Glib::ustring::compose(
            _("机器空间预览仅显示了 %1 / %2 段路径；被机器床面裁切的部分会以近似方式显示。"),
            static_cast<guint64>(included), static_cast<guint64>(total));
    }
    if (clip_approx) {
        return _("机器空间预览中，被机器床面裁切的部分会以近似方式显示。");
    }
    return Glib::ustring::compose(
        _("机器空间预览仅显示了 %1 / %2 段路径；如果图稿过于复杂，请简化图稿以查看全部结果。"),
        static_cast<guint64>(included), static_cast<guint64>(total));
}

} // namespace

Glib::ustring build_grbl_preview_build_error_status(std::string const &err, bool const machine_space)
{
    if (!err.empty()) {
        return Glib::ustring(err);
    }
    return machine_space ? Glib::ustring(_("无法生成机器空间预览。"))
                         : Glib::ustring(_("无法生成文档空间预览。"));
}

Glib::ustring build_grbl_preview_status_note(bool const machine_space, std::size_t const included,
                                             std::size_t const total, bool const clip_approx)
{
    if (clip_approx) {
        return make_preview_summary(machine_space, included, total, true);
    }
    if (total > included) {
        return make_preview_summary(machine_space, included, total, false);
    }
    return {};
}

GrblPreviewOverlayUiPlan make_grbl_preview_overlay_ui_plan(bool const any_preview_active,
                                                           bool const machine_preview_active,
                                                           Glib::ustring const &status_note)
{
    GrblPreviewOverlayUiPlan plan;
    if (!any_preview_active) {
        return plan;
    }

    plan.request_canvas_redraw = true;
    if (!machine_preview_active) {
        return plan;
    }

    plan.build_machine_axis = true;
    plan.post_status = !status_note.empty();
    plan.status = status_note;
    return plan;
}

} // namespace Inkscape::UI::Dialog
