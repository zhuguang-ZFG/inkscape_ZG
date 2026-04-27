// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-presentation.h"

#include <algorithm>
#include <cctype>

#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>

namespace Inkscape::UI::Dialog {

GrblActionButtonsPresentation make_grbl_action_buttons_presentation(GrblRuntimeStateView const &state)
{
    GrblActionButtonsPresentation presentation;

    bool const sending = state.phase == GrblRuntimePhase::gcode_sending;
    bool const cancelling = state.phase == GrblRuntimePhase::gcode_cancelling;

    presentation.send_gcode.label = sending || cancelling ? _("发送中...") : _("发送到机器(_S)");
    presentation.send_from_drawing.label = sending || cancelling ? _("图稿发送中...") : _("从图稿直接发送");
    presentation.read_firmware.label = state.firmware_sync ? _("同步中...") : _("同步绘图机参数");
    presentation.cancel_gcode.label = state.cancel_requested ? _("停止请求中...") : _("停止发送(_C)");
    presentation.cancel_gcode.tooltip = _("设置取消标记；当前这一行仍可能执行完后才会停止发送。");

    if (sending || cancelling) {
        presentation.send_gcode.tooltip = _("当前正在发送编辑器中的 G-code；如需停止，请使用旁边的“停止发送”。");
        presentation.send_from_drawing.tooltip = _("当前正在执行图稿直发；如需停止，请使用“停止发送”。");
    } else {
        presentation.send_gcode.tooltip =
            _("按顺序发送每一条非空行，并在发送下一行前等待 Grbl 返回 ok（或错误）。"
              "长任务执行时，消息日志会更新大致的行数进度。"
              "也可以只从光标所在行开始发送（见上方复选框）。");
        presentation.send_from_drawing.tooltip =
            _("按当前绘图机首选项直接从当前文档生成 G-code，并立刻发送到已连接的绘图机。");
    }

    if (state.firmware_sync) {
        presentation.read_firmware.tooltip = _("正在读取 $I、$G、$#、$$ 并同步方向掩码、床面尺寸等信息。");
    } else {
        presentation.read_firmware.tooltip =
            _("读取 GRBL 固件信息（$I）、当前模态（$G）、偏移（$#）以及全部参数（$$），并同步方向掩码和床面尺寸。");
    }

    return presentation;
}

GrblConnectionPresentation make_grbl_connection_presentation(GrblRuntimeStateView const &state, bool const connected)
{
    GrblConnectionPresentation presentation;
    presentation.serial_controls_sensitive = !state.busy;

    if (state.connecting) {
        presentation.connect.label = _("连接中...");
        presentation.connect.tooltip = _("正在打开连接并探测控制器，请稍候。");
    } else if (connected) {
        presentation.connect.label = _("断开连接");
        presentation.connect.tooltip = _("断开当前绘图机连接。");
    } else {
        presentation.connect.label = _("连接");
        presentation.connect.tooltip = _("连接当前选中的串口或网络控制器。连接成功后会自动读取一轮固件参数。");
    }

    return presentation;
}

Glib::ustring build_grbl_connection_status(Glib::ustring const &device, Glib::ustring const &probe_response)
{
    if (probe_response.empty()) {
        return Glib::ustring::compose(_("已连接到 %1"), device);
    }
    return Glib::ustring::compose(_("已连接到 %1\n控制器：%2"), device, probe_response);
}

Glib::ustring build_grbl_not_connected_status(bool const serial_required)
{
    return serial_required ? Glib::ustring(_("尚未连接串口绘图机。")) : Glib::ustring(_("尚未连接。"));
}

bool grbl_gcode_text_has_content(std::string const &text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char const c) { return !std::isspace(c); });
}

std::string build_grbl_default_gcode_filename(char const *const document_filename)
{
    std::string initial = "plot.nc";
    if (!document_filename || !*document_filename) {
        return initial;
    }

    std::string base = Glib::path_get_basename(document_filename);
    auto const dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base.erase(dot);
    }
    if (!base.empty()) {
        initial = std::move(base) + ".nc";
    }
    return initial;
}

Glib::ustring build_grbl_gcode_load_status(Glib::ustring const &parse_name)
{
    return Glib::ustring::compose(_("已从“%1”载入 G-code"), parse_name);
}

Glib::ustring build_grbl_gcode_save_status(Glib::ustring const &parse_name)
{
    return Glib::ustring::compose(_("G-code 已保存到“%1”"), parse_name);
}

} // namespace Inkscape::UI::Dialog
