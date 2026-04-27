// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared GRBL work-origin command helpers for the control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_WORK_ORIGIN_H
#define INKSCAPE_UI_DIALOG_GRBL_WORK_ORIGIN_H

#include <string>
#include <vector>

#include <glibmm/ustring.h>

namespace Inkscape::UI::Dialog {

enum class GrblWorkOriginAction {
    set_origin_xy,
    goto_work_zero_xy,
    return_after_cancel,
};

struct GrblWorkOriginCommand
{
    std::vector<std::string> lines;
    bool refresh_preview = true;
    Glib::ustring success_status;
};

GrblWorkOriginCommand build_grbl_work_origin_command(GrblWorkOriginAction action);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_WORK_ORIGIN_H
