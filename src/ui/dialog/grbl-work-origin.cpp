// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-work-origin.h"

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {

GrblWorkOriginCommand build_grbl_work_origin_command(GrblWorkOriginAction const action)
{
    GrblWorkOriginCommand command;

    switch (action) {
        case GrblWorkOriginAction::set_origin_xy:
            command.lines = {"G21", "G92 X0 Y0"};
            command.success_status = _("已将当前位置设为工作零点（X0 Y0，未改 Z）。");
            return command;
        case GrblWorkOriginAction::goto_work_zero_xy:
            command.lines = {"G21", "G90", "G0 X0 Y0"};
            command.success_status = _("已移动到工作 XY 零点。");
            return command;
        case GrblWorkOriginAction::return_after_cancel:
            command.lines = {"G21", "G90", "G0 X0 Y0"};
            return command;
        default:
            return command;
    }
}

} // namespace Inkscape::UI::Dialog
