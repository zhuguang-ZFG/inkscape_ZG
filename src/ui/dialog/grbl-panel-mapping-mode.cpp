// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-mapping-prefs.h"

namespace Inkscape::UI::Dialog {
namespace {

constexpr auto k_tool_change_mode_none = "none";
constexpr auto k_tool_change_mode_manual = "manual";
constexpr auto k_tool_change_mode_m6 = "m6";

} // namespace

char const *get_grbl_tool_change_mode_id(bool const auto_pause_between_layers, bool const manual_pen_change,
                                         bool const tool_change_m6)
{
    if (tool_change_m6) {
        return k_tool_change_mode_m6;
    }
    if (auto_pause_between_layers && manual_pen_change) {
        return k_tool_change_mode_manual;
    }
    return k_tool_change_mode_none;
}

void apply_grbl_tool_change_mode_id(std::string const &mode_id, GrblPanelMappingPrefs &prefs)
{
    bool const manual_pen_change = mode_id == k_tool_change_mode_manual;
    bool const tool_change_m6 = mode_id == k_tool_change_mode_m6;
    prefs.auto_pause_between_layers = manual_pen_change || tool_change_m6;
    prefs.manual_pen_change = manual_pen_change;
    prefs.tool_change_m6 = tool_change_m6;
    prefs.tool_change_point = tool_change_m6 && prefs.tool_change_point;
}

} // namespace Inkscape::UI::Dialog
