// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-editor-gcode.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <ios>
#include <sstream>

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {
namespace {

constexpr double k_mm_per_in = 25.4;

void trim_in_place(std::string &s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    auto it = s.begin();
    while (it != s.end() && (*it == ' ' || *it == '\t')) {
        ++it;
    }
    s.erase(s.begin(), it);
}

bool should_skip_gcode_line(std::string const &s)
{
    if (s.empty() || s[0] == ';') {
        return true;
    }
    if (s[0] == '(') {
        auto const end = s.find(')');
        if (end != std::string::npos && end + 1 == s.size()) {
            return true;
        }
    }
    return false;
}

template <typename Func>
void for_each_executable_gcode_line(std::string const &text, Func &&func)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        trim_in_place(line);
        if (should_skip_gcode_line(line)) {
            continue;
        }
        func(line);
    }
}

bool parse_gcode_word_value(std::string const &line, std::size_t &pos, char &letter, double &value)
{
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    if (pos >= line.size()) {
        return false;
    }
    if (!std::isalpha(static_cast<unsigned char>(line[pos]))) {
        ++pos;
        return false;
    }

    letter = static_cast<char>(std::toupper(static_cast<unsigned char>(line[pos++])));
    char *end = nullptr;
    auto const start = line.c_str() + pos;
    value = std::strtod(start, &end);
    if (end == start) {
        return false;
    }
    pos = static_cast<std::size_t>(end - line.c_str());
    return true;
}

} // namespace

bool analyze_editor_gcode_bounds_mm(std::string const &text, EditorGcodeBounds &bounds)
{
    bounds = {};

    double current_x_mm = 0.0;
    double current_y_mm = 0.0;
    bool absolute_mode = true;
    double unit_scale_to_mm = 1.0;

    auto update_bounds = [&](double x_mm, double y_mm) {
        if (!bounds.saw_xy_motion) {
            bounds.saw_xy_motion = true;
            bounds.min_x_mm = bounds.max_x_mm = x_mm;
            bounds.min_y_mm = bounds.max_y_mm = y_mm;
            return;
        }
        bounds.min_x_mm = std::min(bounds.min_x_mm, x_mm);
        bounds.max_x_mm = std::max(bounds.max_x_mm, x_mm);
        bounds.min_y_mm = std::min(bounds.min_y_mm, y_mm);
        bounds.max_y_mm = std::max(bounds.max_y_mm, y_mm);
    };

    for_each_executable_gcode_line(text, [&](std::string const &line) {
        std::size_t pos = 0;
        bool saw_motion_g = false;
        bool has_x = false;
        bool has_y = false;
        double next_x_mm = current_x_mm;
        double next_y_mm = current_y_mm;

        while (pos < line.size()) {
            char letter = '\0';
            double value = 0.0;
            if (!parse_gcode_word_value(line, pos, letter, value)) {
                continue;
            }

            switch (letter) {
                case 'G': {
                    int const g = static_cast<int>(std::lround(value));
                    if (std::fabs(value - g) > 1e-6) {
                        break;
                    }
                    if (g == 90) {
                        absolute_mode = true;
                    } else if (g == 91) {
                        absolute_mode = false;
                    } else if (g == 20) {
                        unit_scale_to_mm = k_mm_per_in;
                    } else if (g == 21) {
                        unit_scale_to_mm = 1.0;
                    } else if (g == 0 || g == 1 || g == 2 || g == 3) {
                        saw_motion_g = true;
                    }
                    break;
                }
                case 'X': {
                    double const x_mm = value * unit_scale_to_mm;
                    next_x_mm = absolute_mode ? x_mm : (current_x_mm + x_mm);
                    has_x = true;
                    break;
                }
                case 'Y': {
                    double const y_mm = value * unit_scale_to_mm;
                    next_y_mm = absolute_mode ? y_mm : (current_y_mm + y_mm);
                    has_y = true;
                    break;
                }
                default:
                    break;
            }
        }

        if (saw_motion_g && (has_x || has_y)) {
            current_x_mm = next_x_mm;
            current_y_mm = next_y_mm;
            update_bounds(current_x_mm, current_y_mm);
        }
    });

    return true;
}

Glib::ustring build_editor_gcode_out_of_bed_message(EditorGcodeBounds const &bounds, double const bed_w_mm,
                                                    double const bed_h_mm)
{
    auto fmt = [](double v) {
        std::ostringstream ss;
        ss.setf(std::ios::fixed);
        ss << std::setprecision(3) << v;
        return ss.str();
    };
    return Glib::ustring::compose(
        _("编辑器中的 G-code 超出了当前机器床面范围。\n"
          "检测到范围: X %1..%2 mm, Y %3..%4 mm\n"
          "当前床面: X 0..%5 mm, Y 0..%6 mm\n\n"
          "如果这份 G-code 是之前按旧映射/旧床面生成的，请先重新“从图稿填充”，"
          "或调整映射后再发送。"),
        fmt(bounds.min_x_mm), fmt(bounds.max_x_mm), fmt(bounds.min_y_mm), fmt(bounds.max_y_mm), fmt(bed_w_mm), fmt(bed_h_mm));
}

bool nearly_equal_mm(double const a, double const b, double const eps)
{
    return std::fabs(a - b) <= eps;
}

} // namespace Inkscape::UI::Dialog
