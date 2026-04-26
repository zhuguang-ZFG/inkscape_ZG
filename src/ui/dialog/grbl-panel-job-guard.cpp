// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-job-guard.h"

#include <algorithm>

#include <glibmm/i18n.h>

#include "ui/dialog/grbl-editor-generation-state.h"

namespace Inkscape::UI::Dialog {

Glib::ustring get_grbl_editor_send_block_reason(GrblEditorSendGuardInput const &input)
{
    if (!input.has_executable_content) {
        return input.send_from_cursor
                   ? Glib::ustring(_("从光标所在行往下没有可发送内容（为空或仅含注释）。"))
                   : Glib::ustring(_("G-code 为空。"));
    }

    if (input.stale_generated_gcode) {
        return build_editor_gcode_stale_mapping_message();
    }

    if (!input.check_bed_bounds || !input.bounds.saw_xy_motion) {
        return {};
    }

    double constexpr eps = 1e-6;
    double const bed_w_mm = std::max(1.0, input.bed_width_mm);
    double const bed_h_mm = std::max(1.0, input.bed_height_mm);
    bool const out_of_bed =
        input.bounds.min_x_mm < -eps || input.bounds.min_y_mm < -eps ||
        input.bounds.max_x_mm > bed_w_mm + eps || input.bounds.max_y_mm > bed_h_mm + eps;
    if (!out_of_bed) {
        return {};
    }

    return build_editor_gcode_out_of_bed_message(input.bounds, bed_w_mm, bed_h_mm);
}

Glib::ustring get_grbl_direct_send_block_reason(GrblDirectSendGuardInput const &input)
{
    switch (input.connection_state) {
        case GrblDirectSendConnectionState::tcp_connected:
            return _("“从图稿直接发送”目前仅支持串口直连的流式发送。网络连接请先“从图稿填充”，再发送编辑器中的 G-code。");
        case GrblDirectSendConnectionState::not_connected:
            return _("尚未连接串口绘图机。");
        case GrblDirectSendConnectionState::ready_serial:
            break;
    }

    if (input.requires_parent_window && !input.has_parent_window) {
        return _("当前无法弹出手动换笔确认窗口，请先使用有父窗口的绘图机工作台发送。");
    }

    return {};
}

} // namespace Inkscape::UI::Dialog
