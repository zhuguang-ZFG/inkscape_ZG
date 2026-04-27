// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared GRBL pen command helpers for the control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PEN_COMMAND_H
#define INKSCAPE_UI_DIALOG_GRBL_PEN_COMMAND_H

#include <string>
#include <vector>

#include <glibmm/ustring.h>

namespace Inkscape::UI::Dialog {

enum class GrblPenMotion {
    up,
    down,
};

struct GrblPenCommand
{
    bool ok = false;
    std::vector<std::string> lines;
    Glib::ustring error;
};

GrblPenCommand build_grbl_pen_command(std::string const &control_mode, GrblPenMotion motion,
                                      std::string const &configured_cmd);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PEN_COMMAND_H
