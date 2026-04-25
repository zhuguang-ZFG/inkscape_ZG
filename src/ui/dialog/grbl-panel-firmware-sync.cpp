// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-firmware-sync.h"

#include "grbl-control-panel.h"

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/spinbutton.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/grbl-link.h"
#include "util/scope_exit.h"

namespace Inkscape::UI::Dialog {
namespace {

struct GrblFirmwareSnapshot {
    bool has_direction_mask = false;
    int direction_mask = 0;
    bool has_x_travel = false;
    bool has_y_travel = false;
    double x_travel_mm = 0.0;
    double y_travel_mm = 0.0;
    Glib::ustring display_text;
};

bool parse_grbl_setting_line(std::string const &line, int &code_out, std::string &value_out)
{
    if (line.size() < 4 || line[0] != '$') {
        return false;
    }
    auto const eq = line.find('=');
    if (eq == std::string::npos || eq <= 1 || eq + 1 >= line.size()) {
        return false;
    }
    try {
        code_out = std::stoi(line.substr(1, eq - 1));
    } catch (...) {
        return false;
    }
    value_out = line.substr(eq + 1);
    return true;
}

bool parse_double_c(std::string const &text, double &value_out)
{
    try {
        size_t idx = 0;
        auto const parsed = std::stod(text, &idx);
        if (idx != text.size()) {
            return false;
        }
        value_out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool update_check_if_needed(Gtk::CheckButton &button, bool const value)
{
    if (button.get_active() == value) {
        return false;
    }
    button.set_active(value);
    return true;
}

bool update_spin_if_needed(Gtk::SpinButton &spin, double const value, double const epsilon = 1e-6)
{
    if (std::abs(spin.get_value() - value) <= epsilon) {
        return false;
    }
    spin.set_value(value);
    return true;
}

Glib::ustring build_firmware_snapshot_text(std::vector<std::string> const &info_lines,
                                           std::vector<std::string> const &modal_lines,
                                           std::vector<std::string> const &offset_lines,
                                           std::vector<std::string> const &setting_lines,
                                           std::vector<std::string> const &errors)
{
    std::ostringstream out;
    auto append_section = [&out](char const *title, std::vector<std::string> const &lines) {
        if (lines.empty()) {
            return;
        }
        if (out.tellp() > 0) {
            out << "\n\n";
        }
        out << title << "\n";
        for (auto const &line : lines) {
            out << line << "\n";
        }
    };

    append_section("[$I]", info_lines);
    append_section("[$G]", modal_lines);
    append_section("[$#]", offset_lines);
    append_section("[$$]", setting_lines);
    append_section("[errors]", errors);

    return out.str();
}

} // namespace

void GrblPanelFirmwareSync::run(GrblControlPanel &panel, std::atomic<bool> const &stop)
{
    scope_exit const finish_sync{[&panel] {
        Glib::signal_idle().connect_once(sigc::track_object([&panel] {
            panel.set_firmware_syncing_state(false);
            if (panel.is_connect_active()) {
                panel.ensure_machine_status_poll(true);
            }
            panel.schedule_plot_feedback_refresh(false);
        }, panel));
    }};

    if (stop.load(std::memory_order_acquire)) {
        return;
    }

    auto query_lines_locked = [&panel, &stop](std::string const &command, std::vector<std::string> &lines_out,
                                              std::string &err_out) -> bool {
        lines_out.clear();
        auto *link = panel.grbl_link();
        if (stop.load(std::memory_order_acquire) || !(link && link->is_open())) {
            err_out = "not connected";
            return false;
        }

        link->purge_io();
        if (stop.load(std::memory_order_acquire)) {
            err_out = "cancelled";
            return false;
        }

        bool const sent = link->write_line(command);
        if (!sent) {
            err_out = "serial write failed";
            return false;
        }

        for (;;) {
            if (stop.load(std::memory_order_acquire)) {
                err_out = "cancelled";
                return false;
            }
            std::string line;
            if (!link->read_line(line, 1800)) {
                err_out = "timeout waiting for controller response";
                return false;
            }
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
                line.pop_back();
            }
            auto it = line.begin();
            while (it != line.end() && (*it == ' ' || *it == '\t')) {
                ++it;
            }
            line.erase(line.begin(), it);
            if (line.empty()) {
                continue;
            }
            if (line == command) {
                continue;
            }
            if (line == "ok") {
                return true;
            }
            if (Inkscape::Axidraw::grbl_is_error_line(line)) {
                err_out = line;
                return false;
            }
            lines_out.push_back(std::move(line));
        }
    };

    std::vector<std::string> info_lines;
    std::vector<std::string> modal_lines;
    std::vector<std::string> offset_lines;
    std::vector<std::string> setting_lines;
    std::vector<std::string> errors;
    GrblFirmwareSnapshot snapshot;

    if (!panel.with_locked_open_link(stop, [&] {
        auto run_query = [&](std::string const &command, std::vector<std::string> &dest) {
            std::string err;
            if (!query_lines_locked(command, dest, err)) {
                errors.push_back(command + ": " + Inkscape::Axidraw::grbl_error_to_user_message(err));
            }
        };

        run_query("$I", info_lines);
        run_query("$G", modal_lines);
        run_query("$#", offset_lines);
        run_query("$$", setting_lines);
    })) {
        return;
    }

    for (auto const &line : setting_lines) {
        int code = 0;
        std::string value;
        if (!parse_grbl_setting_line(line, code, value)) {
            continue;
        }
        if (code == 3) {
            try {
                snapshot.direction_mask = std::stoi(value);
                snapshot.has_direction_mask = true;
            } catch (...) {
            }
        } else if (code == 130) {
            snapshot.has_x_travel = parse_double_c(value, snapshot.x_travel_mm);
        } else if (code == 131) {
            snapshot.has_y_travel = parse_double_c(value, snapshot.y_travel_mm);
        }
    }

    snapshot.display_text = build_firmware_snapshot_text(info_lines, modal_lines, offset_lines, setting_lines, errors);

    if (stop.load(std::memory_order_acquire)) {
        return;
    }

    Glib::signal_idle().connect_once(sigc::track_object([&panel, snapshot] {
        auto apply_snapshot_to_ui = [&panel](GrblFirmwareSnapshot const &snapshot_in, bool &page_synced_out,
                                             bool &unit_synced_out) {
            bool changed_local = false;
            panel.set_mapping_sync_suspended(true);
            if (snapshot_in.has_direction_mask) {
                changed_local = update_check_if_needed(panel._chk_invert_x, (snapshot_in.direction_mask & 0x1) != 0) || changed_local;
                changed_local = update_check_if_needed(panel._chk_invert_y, (snapshot_in.direction_mask & 0x2) != 0) || changed_local;
            }
            if (snapshot_in.has_x_travel) {
                changed_local = update_spin_if_needed(panel._bed_width_spin, snapshot_in.x_travel_mm) || changed_local;
            }
            if (snapshot_in.has_y_travel) {
                changed_local = update_spin_if_needed(panel._bed_depth_spin, snapshot_in.y_travel_mm) || changed_local;
            }
            panel.set_mapping_sync_suspended(false);

            page_synced_out = false;
            unit_synced_out = false;
            if (panel.should_sync_page_to_bed_on_firmware_read() && snapshot_in.has_x_travel && snapshot_in.has_y_travel) {
                if (auto *doc = panel.getDocument()) {
                    page_synced_out = panel.sync_document_page_to_bed_mm(doc, snapshot_in.x_travel_mm,
                                                                         snapshot_in.y_travel_mm, unit_synced_out);
                    if (page_synced_out) {
                        panel.finalize_document_geometry_change(doc, GrblControlPanel::DocumentGeometryChange::sync_page_to_bed);
                    }
                }
            }

            return changed_local;
        };
        auto build_sync_status = [](GrblFirmwareSnapshot const &snapshot_in, bool page_synced_in, bool unit_synced_in) {
            std::vector<Glib::ustring> notes;
            if (snapshot_in.has_direction_mask) {
                notes.emplace_back(Glib::ustring::compose(_("已同步方向反转掩码 $3=%1"), snapshot_in.direction_mask));
            }
            if (snapshot_in.has_x_travel || snapshot_in.has_y_travel) {
                if (snapshot_in.has_x_travel && snapshot_in.has_y_travel) {
                    notes.emplace_back(Glib::ustring::compose(_("已同步床面尺寸 X=%1 mm, Y=%2 mm"),
                                                              snapshot_in.x_travel_mm, snapshot_in.y_travel_mm));
                } else if (snapshot_in.has_x_travel) {
                    notes.emplace_back(Glib::ustring::compose(_("已同步床面宽度 X=%1 mm"), snapshot_in.x_travel_mm));
                } else {
                    notes.emplace_back(Glib::ustring::compose(_("已同步床面深度 Y=%1 mm"), snapshot_in.y_travel_mm));
                }
            } else {
                notes.emplace_back(_("未从固件读取到 $130 / $131 行程参数，因此没有同步页面尺寸。"));
            }
            if (page_synced_in) {
                notes.emplace_back(
                    Glib::ustring::compose(_("已将当前页面尺寸同步为 %1 x %2 mm"), snapshot_in.x_travel_mm, snapshot_in.y_travel_mm));
            }
            if (unit_synced_in) {
                notes.emplace_back(_("已将文档单位同步为 mm"));
            }
            if (notes.empty()) {
                return Glib::ustring(_("已读取固件参数。"));
            }

            std::ostringstream msg;
            msg << _("已读取固件参数。");
            for (auto const &note : notes) {
                msg << "\n" << note.raw();
            }
            return Glib::ustring(msg.str());
        };

        panel.set_firmware_info_text(snapshot.display_text);

        bool page_synced = false;
        bool unit_synced = false;
        bool const changed = apply_snapshot_to_ui(snapshot, page_synced, unit_synced);

        if (changed) {
            panel.save_mapping_preferences_from_ui(true);
        } else {
            panel.schedule_plot_feedback_refresh(true);
        }
        panel.post_status(build_sync_status(snapshot, page_synced, unit_synced), false);
    }, panel));
}

} // namespace Inkscape::UI::Dialog
