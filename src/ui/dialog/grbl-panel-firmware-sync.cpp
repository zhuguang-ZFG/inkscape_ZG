// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-firmware-sync.h"
#include "grbl-panel-firmware-sync-state.h"

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <glibmm/i18n.h>
#include <glibmm/main.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/grbl-link.h"
#include "util/scope_exit.h"

namespace Inkscape::UI::Dialog {
namespace {

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

bool is_bracket_info_line(std::string const &line)
{
    return !line.empty() && line.front() == '[';
}

} // namespace

void GrblPanelFirmwareSync::run(GrblPanelFirmwareSyncContext const &context, std::atomic<bool> const &stop)
{
    scope_exit const finish_sync{[&context] { context.finish_sync_ui(); }};

    if (stop.load(std::memory_order_acquire)) {
        return;
    }

    auto wake_link_locked = [&context, &stop]() -> bool {
        auto *link = context.link;
        if (stop.load(std::memory_order_acquire) || !(link && link->is_open())) {
            return false;
        }

        link->purge_io();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        Inkscape::Axidraw::grbl_debug_log_write("firmware_sync", "\\r\\n");
        link->write_line("");
        Inkscape::Axidraw::grbl_debug_log_write("firmware_sync", "\\r\\n");
        link->write_line("");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        std::string junk;
        for (int i = 0; i < 24; ++i) {
            if (!link->read_line(junk, 80)) {
                break;
            }
            if (stop.load(std::memory_order_acquire)) {
                return false;
            }
        }
        return !stop.load(std::memory_order_acquire);
    };

    auto query_lines_locked = [&context, &stop](std::string const &command, std::vector<std::string> &lines_out,
                                                std::string &err_out) -> bool {
        lines_out.clear();
        auto *link = context.link;
        if (stop.load(std::memory_order_acquire) || !(link && link->is_open())) {
            err_out = "not connected";
            return false;
        }

        bool const sent = link->write_line(command);
        Inkscape::Axidraw::grbl_debug_log_write("firmware_sync", command);
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
            if (!link->read_line(line, 4000)) {
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
            if (line.empty() || line == command) {
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

    if (!context.with_locked_open_link(stop, [&] {
        if (!wake_link_locked()) {
            return;
        }

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

        if (info_lines.empty() && modal_lines.empty() && offset_lines.empty() && setting_lines.empty() &&
            !stop.load(std::memory_order_acquire)) {
            errors.clear();

            auto *link = context.link;
            if (link && link->is_open()) {
                char const ctrl_x = 0x18;
                Inkscape::Axidraw::grbl_debug_log_write("firmware_sync", "\\x18");
                if (link->write_bytes(&ctrl_x, 1)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                    std::string line;
                    for (int i = 0; i < 16; ++i) {
                        if (!link->read_line(line, 120)) {
                            break;
                        }
                        if (!line.empty()) {
                            info_lines.push_back(line);
                        }
                    }

                    if (wake_link_locked()) {
                        std::string err;
                        std::vector<std::string> combined_lines;
                        if (!query_lines_locked("$$", combined_lines, err)) {
                            errors.push_back("$$: " + Inkscape::Axidraw::grbl_error_to_user_message(err));
                        } else {
                            for (auto &entry : combined_lines) {
                                int code = 0;
                                std::string value;
                                if (parse_grbl_setting_line(entry, code, value)) {
                                    setting_lines.push_back(entry);
                                } else if (is_bracket_info_line(entry)) {
                                    info_lines.push_back(entry);
                                } else if (!entry.empty() && entry.front() == '<') {
                                    offset_lines.push_back(entry);
                                } else {
                                    info_lines.push_back(entry);
                                }
                            }
                        }
                    }
                } else {
                    errors.emplace_back(_("兼容模式软复位失败。"));
                }
            }
        }
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

    Glib::signal_idle().connect_once([context, snapshot] {
        context.set_firmware_info_text(snapshot.display_text);

        auto const apply_result = context.apply_snapshot_to_ui(snapshot);
        auto const ui_plan = make_grbl_firmware_sync_ui_plan(snapshot, apply_result);

        if (ui_plan.save_mapping_preferences) {
            context.save_mapping_preferences(true);
        }
        if (ui_plan.schedule_plot_feedback_refresh) {
            context.schedule_plot_feedback_refresh(true);
        }
        context.post_status(ui_plan.status, false);
    });
}

} // namespace Inkscape::UI::Dialog
