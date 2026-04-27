// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-firmware-sync.h"
#include "grbl-panel-firmware-sync-state.h"

#include <chrono>
#include <cctype>
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

bool starts_with(std::string const &text, std::string const &prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string trim_copy(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

bool parse_int_c(std::string const &text, int &value_out)
{
    try {
        size_t idx = 0;
        auto const parsed = std::stoi(text, &idx);
        if (idx != text.size()) {
            return false;
        }
        value_out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::string find_ipv4_in_text(std::string const &text)
{
    auto is_digit = [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch)) != 0;
    };
    auto is_boundary = [](std::string const &s, std::size_t pos) {
        if (pos >= s.size()) {
            return true;
        }
        char const ch = s[pos];
        return !(std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_');
    };

    for (std::size_t start = 0; start < text.size(); ++start) {
        if (!is_digit(text[start])) {
            continue;
        }
        if (start > 0 && !is_boundary(text, start - 1)) {
            continue;
        }

        std::size_t pos = start;
        std::string ip;
        bool ok = true;
        for (int part = 0; part < 4; ++part) {
            if (pos >= text.size() || !is_digit(text[pos])) {
                ok = false;
                break;
            }
            int value = 0;
            std::size_t digits = 0;
            while (pos < text.size() && is_digit(text[pos]) && digits < 3) {
                value = value * 10 + (text[pos] - '0');
                ++pos;
                ++digits;
            }
            if (digits == 0 || value > 255) {
                ok = false;
                break;
            }
            if (pos < text.size() && is_digit(text[pos])) {
                ok = false;
                break;
            }
            ip += std::to_string(value);
            if (part < 3) {
                if (pos >= text.size() || text[pos] != '.') {
                    ok = false;
                    break;
                }
                ip.push_back('.');
                ++pos;
            }
        }

        if (ok && is_boundary(text, pos)) {
            return ip;
        }
    }

    return {};
}

void parse_esp_snapshot_fields(std::vector<std::string> const &esp_version_lines,
                               std::vector<std::string> const &esp_status_lines,
                               GrblFirmwareSnapshot &snapshot)
{
    for (auto const &line : esp_status_lines) {
        if (!snapshot.has_esp_data_port && starts_with(line, "Data port:")) {
            int value = 0;
            if (parse_int_c(trim_copy(line.substr(std::string("Data port:").size())), value) && value > 0) {
                snapshot.esp_data_port = value;
                snapshot.has_esp_data_port = true;
            }
            continue;
        }
        if (!snapshot.has_esp_hostname && starts_with(line, "Hostname:")) {
            auto value = trim_copy(line.substr(std::string("Hostname:").size()));
            if (!value.empty()) {
                snapshot.esp_hostname = value;
                snapshot.has_esp_hostname = true;
            }
            continue;
        }
        if (!snapshot.has_esp_wifi_mode && starts_with(line, "Current WiFi Mode:")) {
            auto value = trim_copy(line.substr(std::string("Current WiFi Mode:").size()));
            if (!value.empty()) {
                snapshot.esp_wifi_mode = value;
                snapshot.has_esp_wifi_mode = true;
            }
            continue;
        }
        if (!snapshot.has_esp_ip && starts_with(line, "IP:")) {
            auto value = find_ipv4_in_text(line);
            if (!value.empty()) {
                snapshot.esp_ip = value;
                snapshot.has_esp_ip = true;
            }
        }
    }

    for (auto const &line : esp_version_lines) {
        if (!snapshot.has_esp_hostname) {
            auto const marker = line.find("# hostname:");
            if (marker != std::string::npos) {
                auto value = trim_copy(line.substr(marker + std::string("# hostname:").size()));
                auto const suffix = value.find('(');
                if (suffix != std::string::npos) {
                    value = trim_copy(value.substr(0, suffix));
                }
                if (!value.empty()) {
                    snapshot.esp_hostname = value;
                    snapshot.has_esp_hostname = true;
                }
            }
        }
        if (!snapshot.has_esp_ip) {
            auto value = find_ipv4_in_text(line);
            if (!value.empty()) {
                snapshot.esp_ip = value;
                snapshot.has_esp_ip = true;
            }
        }
    }
}

Glib::ustring build_firmware_snapshot_text(std::vector<std::string> const &info_lines,
                                           std::vector<std::string> const &modal_lines,
                                           std::vector<std::string> const &offset_lines,
                                           std::vector<std::string> const &setting_lines,
                                           std::vector<std::string> const &esp_version_lines,
                                           std::vector<std::string> const &esp_status_lines,
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
    append_section("[ESP800]", esp_version_lines);
    append_section("[ESP420]", esp_status_lines);
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
    std::vector<std::string> esp_version_lines;
    std::vector<std::string> esp_status_lines;
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

        if (!context.esp_admin_password.empty()) {
            run_query("[ESP800]pwd=" + context.esp_admin_password, esp_version_lines);
            run_query("[ESP420]pwd=" + context.esp_admin_password, esp_status_lines);
        }

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

    parse_esp_snapshot_fields(esp_version_lines, esp_status_lines, snapshot);

    snapshot.display_text = build_firmware_snapshot_text(
        info_lines, modal_lines, offset_lines, setting_lines, esp_version_lines, esp_status_lines, errors);

    if (stop.load(std::memory_order_acquire)) {
        return;
    }

    auto apply_snapshot = [context, snapshot] {
        context.set_firmware_info_text(snapshot.display_text);

        auto const apply_result = context.apply_snapshot_to_ui(snapshot);
        auto const ui_plan = make_grbl_firmware_sync_ui_plan(snapshot, apply_result);
        auto status = ui_plan.status;
        if (snapshot.has_esp_wifi_mode || snapshot.has_esp_hostname || snapshot.has_esp_ip || snapshot.has_esp_data_port) {
            std::ostringstream esp;
            esp << "ESP";
            if (snapshot.has_esp_wifi_mode) {
                esp << " mode=" << snapshot.esp_wifi_mode;
            }
            if (snapshot.has_esp_hostname) {
                esp << " host=" << snapshot.esp_hostname;
            }
            if (snapshot.has_esp_ip) {
                esp << " ip=" << snapshot.esp_ip;
            }
            if (snapshot.has_esp_data_port) {
                esp << " port=" << snapshot.esp_data_port;
            }
            status += "\n";
            status += esp.str();
        }

        if (ui_plan.save_mapping_preferences) {
            context.save_mapping_preferences(true);
        }
        if (ui_plan.schedule_plot_feedback_refresh) {
            context.schedule_plot_feedback_refresh(true);
        }
        context.post_status(status, false);
    };
    if (context.dispatch_to_ui) {
        context.dispatch_to_ui(std::move(apply_snapshot));
    } else {
        Glib::signal_idle().connect_once(std::move(apply_snapshot));
    }
}

} // namespace Inkscape::UI::Dialog
