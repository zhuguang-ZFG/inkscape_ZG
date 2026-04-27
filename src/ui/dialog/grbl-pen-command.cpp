// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-pen-command.h"

#include "grbl-editor-gcode.h"

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {

GrblPenCommand build_grbl_pen_command(std::string const &control_mode, GrblPenMotion const motion,
                                      std::string const &configured_cmd)
{
    GrblPenCommand result;

    if (control_mode == "m3m5" || control_mode == "M3M5") {
        result.ok = true;
        result.lines.emplace_back(motion == GrblPenMotion::up ? "M5" : "M3 S1000");
        return result;
    }

    for_each_executable_grbl_gcode_line(configured_cmd, [&](std::string const &line) {
        result.lines.push_back(line);
        return true;
    });

    if (result.lines.empty()) {
        result.error = motion == GrblPenMotion::up ? _("抬笔命令（首选项中设置）为空")
                                                   : _("落笔命令（首选项中设置）为空");
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace Inkscape::UI::Dialog
