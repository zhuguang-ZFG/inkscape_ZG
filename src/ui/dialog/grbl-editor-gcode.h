// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared editor G-code analysis helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_EDITOR_GCODE_H
#define INKSCAPE_UI_DIALOG_GRBL_EDITOR_GCODE_H

#include <functional>
#include <string>

#include <glibmm/ustring.h>

namespace Inkscape::UI::Dialog {

struct EditorGcodeBounds
{
    bool saw_xy_motion = false;
    double min_x_mm = 0.0;
    double max_x_mm = 0.0;
    double min_y_mm = 0.0;
    double max_y_mm = 0.0;
};

void trim_grbl_gcode_line_in_place(std::string &line);
bool is_skippable_grbl_gcode_line(std::string const &line);
bool normalize_executable_grbl_gcode_line(std::string &line);
std::size_t for_each_executable_grbl_gcode_line(std::string const &text,
                                                std::function<bool(std::string const &)> const &func);
bool analyze_editor_gcode_bounds_mm(std::string const &text, EditorGcodeBounds &bounds);
Glib::ustring build_editor_gcode_out_of_bed_message(EditorGcodeBounds const &bounds, double bed_w_mm, double bed_h_mm);
std::size_t count_executable_editor_gcode_lines(std::string const &text, std::size_t max_lines);
bool nearly_equal_mm(double a, double b, double eps = 1e-6);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_EDITOR_GCODE_H
