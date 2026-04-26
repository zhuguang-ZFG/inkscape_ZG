// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Serial GRBL / axis jog control (dockable dialog).
 *
 * \par Reference (decompiled Java only)
 * From kxnx `tools/dump_agent/windows_reverse/decompiled_java/com/kvenjoy/drawsoft/lib/remote/pck/PrintPck.java`
 * and `com/kvenjoy/drawsoft/lib/e/f.java` (`a(String s)` is true when `s == null || s.length() == 0`):
 *   - `public String singleGcode;` 鈥?in `write(b)`, if `f.a(this.uuid)` (uuid empty) is true, the
 *     code writes byte `0` then the UTF-8 bytes of `singleGcode` (length-prefixed via `b2.b` / `b2.a`);
 *   - if `!f.a(this.uuid)` (uuid not empty), it writes byte `1`, then `uuid` and `public int repeat;`
 *     and returns (no `singleGcode` in that branch in the decompiled source).
 * This dialog does not implement that packet format: it uses a local `SerialPort` and `grbl_send_line`
 * in C++ only; line splitting and comments are handled below without claiming parity with the Java host.
 */

#include "grbl-control-panel.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#include <glib.h>
#include <giomm/liststore.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/textbuffer.h>
#include <gtkmm/window.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/ustring.h>

#include <2geom/pathvector.h>

#include "axidraw/device/grbl-client.h"
#include "axidraw/device/grbl-link.h"
#include "axidraw/device/serial-port-scan.h"
#include "axidraw/device/serial-port.h"
#include "axidraw/device/tcp-port.h"
#include "axidraw/pipeline/grbl-export.h"
#include "desktop.h"
#include "display/control/canvas-item-text.h"
#include "document.h"
#include "document-undo.h"
#include "io/sys.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "preferences.h"
#include "style-enums.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog/grbl-editor-gcode.h"
#include "ui/dialog/grbl-editor-generation-state.h"
#include "ui/dialog/grbl-panel-connect-attempt.h"
#include "ui/dialog/grbl-panel-connection-state.h"
#include "ui/dialog/grbl-panel-feedback-presentation.h"
#include "ui/dialog/grbl-panel-firmware-sync-state.h"
#include "ui/dialog/grbl-panel-document-layout.h"
#include "ui/dialog/grbl-panel-feedback-state.h"
#include "ui/dialog/grbl-panel-mapping-prefs.h"
#include "ui/dialog/grbl-panel-mapping-state.h"
#include "ui/dialog/grbl-panel-summary-presentation.h"
#include "ui/dialog/grbl-runtime-state.h"
#include "ui/dialog/grbl-panel-firmware-sync.h"
#include "ui/dialog/grbl-panel-workers.h"
#include "ui/dialog/grbl-panel-sender.h"
#include "ui/dialog-run.h"
#include "ui/pack.h"
#include "ui/widget/canvas.h"
#include "util/scope_exit.h"
#include "util/units.h"
#include "xml/node.h"

using Inkscape::Axidraw::grbl_begin_plot_waits;
using Inkscape::Axidraw::grbl_end_plot_waits;
using Inkscape::Axidraw::grbl_error_user_cancelled;
using Inkscape::Axidraw::build_grbl_plot_gcode_string;
using Inkscape::Axidraw::build_grbl_plot_machine_preview_pathvector_in_doc_space;
using Inkscape::Axidraw::build_grbl_plot_preview_pathvector;
using Inkscape::Axidraw::GrblPlotStats;
using Inkscape::Axidraw::analyze_grbl_plot;
using Inkscape::Axidraw::export_paths_to_grbl;
using Inkscape::Axidraw::grbl_export_params_from_preferences;
using Inkscape::Axidraw::SerialPort;
using Inkscape::CanvasItemBpath;
using Inkscape::CanvasItemText;
using Inkscape::choose_file_open;
using Inkscape::choose_file_save;

namespace {

void append_axis_arrow(Geom::PathVector &paths, Geom::Point const &from, Geom::Point const &to, double head_len, double head_width)
{
    Geom::Path shaft(from);
    shaft.appendNew<Geom::LineSegment>(to);
    paths.push_back(shaft);
    auto const dx = to[Geom::X] - from[Geom::X];
    auto const dy = to[Geom::Y] - from[Geom::Y];
    auto const len = std::hypot(dx, dy);
    if (len > 1e-6) {
        Geom::Point const dir(dx / len, dy / len);
        Geom::Point const normal(-dir[Geom::Y], dir[Geom::X]);
        Geom::Point const base = to - dir * head_len;
        Geom::Path left(base + normal * head_width);
        left.appendNew<Geom::LineSegment>(to);
        paths.push_back(left);
        Geom::Path right(base - normal * head_width);
        right.appendNew<Geom::LineSegment>(to);
        paths.push_back(right);
    }
}

Geom::Point machine_axis_direction(bool swap_xy, bool invert_x, bool invert_y, bool x_axis)
{
    if (!swap_xy) {
        return x_axis
            ? Geom::Point(invert_x ? -1.0 : 1.0, 0.0)
            : Geom::Point(0.0, invert_y ? -1.0 : 1.0);
    }
    return x_axis
        ? Geom::Point(0.0, invert_x ? -1.0 : 1.0)
        : Geom::Point(invert_y ? -1.0 : 1.0, 0.0);
}


bool send_link_lines(Inkscape::Axidraw::GrblLink &link, std::initializer_list<std::string_view> lines, std::string &err_out)
{
    return Inkscape::Axidraw::grbl_send_lines(
        [&link](std::string const &line, std::string &line_err_out) {
            return link.send_line_wait_ok(line, line_err_out);
        },
        lines, err_out);
}

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

/// Skip empty, `;` comments, and parenthesis-only comment lines; keep inline `(鈥?` on G-code.
bool should_skip_gcode_line(std::string const &s)
{
    if (s.empty()) {
        return true;
    }
    if (s[0] == ';') {
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

std::string detect_radio_mode_from_reply(std::string reply)
{
    for (auto &c : reply) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    auto has_token = [&](char const *tok) {
        std::string const t(tok);
        auto p = reply.find(t);
        if (p == std::string::npos) {
            return false;
        }
        auto boundary = [&](std::size_t idx) {
            if (idx >= reply.size()) {
                return true;
            }
            char const ch = reply[idx];
            return !(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_');
        };
        bool const left_ok = (p == 0) ? true : boundary(p - 1);
        bool const right_ok = boundary(p + t.size());
        return left_ok && right_ok;
    };
    if (has_token("STA")) return "STA";
    if (has_token("AP")) return "AP";
    if (has_token("BT")) return "BT";
    if (has_token("OFF") || has_token("NONE")) return "OFF";
    return {};
}

constexpr std::size_t k_max_gcode_stream_lines = 200000;

constexpr std::size_t k_preview_max_strokes = 12000;

constexpr auto k_pref_device = "/options/grbl/serial-device";
constexpr auto k_pref_baud = "/options/grbl/baud";
constexpr auto k_pref_draw = "/options/grbl/feed-draw-mmmin";
constexpr auto k_pref_travel = "/options/grbl/feed-travel-mmmin";
constexpr auto k_pref_pen_control = "/options/grbl/pen-control";
constexpr auto k_pref_pen_up = "/options/grbl/pen-up-cmd";
constexpr auto k_pref_pen_down = "/options/grbl/pen-down-cmd";
constexpr auto k_pref_pen_up_delay = "/options/grbl/pen-up-delay-ms";
constexpr auto k_pref_pen_down_delay = "/options/grbl/pen-down-delay-ms";
constexpr auto k_pref_limit_layer = "/options/grbl/limit-to-current-layer";
constexpr auto k_pref_sync_page_to_bed = "/options/grbl/sync-page-to-bed-on-firmware-read";
constexpr auto k_pref_net_host = "/options/grbl/net-host";
constexpr auto k_pref_net_port = "/options/grbl/net-port";
constexpr auto k_pref_swap_xy = "/options/grbl/swap-xy";
constexpr auto k_pref_invert_x = "/options/grbl/invert-x";
constexpr auto k_pref_invert_y = "/options/grbl/invert-y";
constexpr auto k_pref_flip_y = "/options/grbl/flip-y-canvas";
constexpr auto k_pref_align_origin = "/options/grbl/align-content-min";
constexpr auto k_pref_clip_bed = "/options/grbl/clip-to-machine-bed";
constexpr auto k_pref_clip_bed_migration_v1 = "/options/grbl/migrations/clip-to-machine-bed-default-v1";
constexpr auto k_pref_bed_width = "/options/grbl/machine-bed-width-mm";
constexpr auto k_pref_bed_depth = "/options/grbl/machine-bed-depth-mm";
constexpr auto k_pref_long_pen_up = "/options/grbl/enable-long-pen-up";
constexpr auto k_pref_long_pen_up_mm = "/options/grbl/long-pen-up-mm";
constexpr auto k_pref_long_move_dist = "/options/grbl/long-move-dist-mm";
constexpr auto k_pref_near_connect = "/options/grbl/enable-near-connect";
constexpr auto k_pref_near_connect_dist = "/options/grbl/near-connect-distance-mm";
constexpr auto k_pref_sparse_sampling = "/options/grbl/enable-sparse-stroke-sampling";
constexpr auto k_pref_sparse_keep_every = "/options/grbl/sparse-keep-every";
constexpr auto k_pref_auto_pause_between_layers = "/options/grbl/auto-pause-between-layers";
constexpr auto k_pref_manual_pen_change = "/options/grbl/manual-pen-change";
constexpr auto k_pref_pen_change_to_home = "/options/grbl/pen-change-to-home";
constexpr auto k_pref_pen_change_prompt = "/options/grbl/pen-change-prompt";
constexpr auto k_pref_tool_change_m6 = "/options/grbl/enable-layer-tool-change-m6";
constexpr auto k_pref_tool_change_point = "/options/grbl/tool-change-use-point";
constexpr auto k_pref_tool_change_x = "/options/grbl/tool-change-x-mm";
constexpr auto k_pref_tool_change_y = "/options/grbl/tool-change-y-mm";
constexpr auto k_pref_start_gcode = "/options/grbl/start-gcode";
constexpr auto k_pref_end_gcode = "/options/grbl/end-gcode";

void migrate_clip_to_machine_bed_default(Inkscape::Preferences *prefs)
{
    if (!prefs || prefs->getEntry(k_pref_clip_bed_migration_v1).isSet()) {
        return;
    }

    prefs->setBool(k_pref_clip_bed, true);
    prefs->setBool(k_pref_clip_bed_migration_v1, true);
    prefs->save();
}

Inkscape::Axidraw::GrblExportParams make_export_params_from_preferences()
{
    auto *prefs = Inkscape::Preferences::get();
    Inkscape::Axidraw::GrblExportParams params;
    grbl_export_params_from_preferences(prefs, params);
    return params;
}

void set_layout_scale_summary_from_stats(Gtk::Label &label, Inkscape::Axidraw::GrblPlotStats const &stats,
                                         double bed_width_mm, double bed_height_mm)
{
    label.set_markup(Inkscape::UI::Dialog::build_grbl_layout_scale_summary_markup(stats, bed_width_mm, bed_height_mm));
}

Geom::PathVector transform_pathvector_to_desktop(Geom::PathVector const &paths, Geom::Affine const &affine)
{
    Geom::PathVector transformed;
    for (auto const &path : paths) {
        transformed.push_back(path * affine);
    }
    return transformed;
}

void configure_preview_overlay(CanvasItemBpath &overlay, uint32_t const stroke, double const stroke_width)
{
    overlay.set_stroke(stroke);
    overlay.set_fill(0x00000000, SP_WIND_RULE_NONZERO);
    overlay.set_stroke_width(stroke_width);
    overlay.set_visible(true);
}

CanvasItemPtr<CanvasItemText> make_preview_axis_label(SPDesktop *desktop, Geom::Point const &pos, Glib::ustring const &text,
                                                      uint32_t const bg)
{
    auto label = make_canvasitem<CanvasItemText>(desktop->getCanvasTemp(), pos, text);
    label->set_fontsize(11.0);
    label->set_background(bg);
    label->set_border(4.0);
    label->set_visible(true);
    return label;
}

/// Last folder for G-code save/open dialogs in this panel.
constexpr auto k_pref_save_gcode_dir = "/dialogs/grblcontrol/save_gcode_dir";
constexpr std::size_t k_max_gcode_editor_bytes = 32u * 1024u * 1024u;

constexpr auto k_tool_change_mode_none = "none";
constexpr auto k_tool_change_mode_manual = "manual";
constexpr auto k_tool_change_mode_m6 = "m6";

struct GrblFirmwareSnapshot {
    bool has_direction_mask = false;
    int direction_mask = 0;
    bool has_x_travel = false;
    double x_travel_mm = 0;
    bool has_y_travel = false;
    double y_travel_mm = 0;
    Glib::ustring display_text;
};

} // namespace

namespace Inkscape::UI::Dialog {

GrblControlPanel::GrblControlPanel()
    : DialogBase("/dialogs/grblcontrol", "GrblControl")
    , _btn_mech_home(_("机械归零(_H)（$H）"))
    , _btn_yp(_("_Y+"))
    , _btn_set_origin(_("设为原点(_E)（G92）"))
    , _btn_xm(_("_X-"))
    , _btn_goto_work_zero(_("前往工作 XY 零点(_O)"))
    , _btn_xp(_("_X+"))
    , _btn_reset(_("重置控制器(_R)"))
    , _btn_ym(_("_Y-"))
    , _btn_pen_up(_("抬笔(_P)"))
    , _btn_pen_down(_("落笔(_D)"))
    , _btn_motors(_("电机休眠(_O)（$SLP）"))
    , _btn_clear_alarm(_("清除报警(_M)（$X）"))
    , _btn_fit_to_bed(_("缩放到机器行程内"))
    , _btn_center_to_bed(_("居中到机器行程"))
    , _btn_restore_page_size(_("恢复原页面尺寸"))
    , _btn_load_gcode(_("载入 G-code(_L)..."))
    , _btn_fill_from_drawing(_("从图稿填充(_D)"))
    , _btn_save_gcode(_("G-code 另存为(_A)..."))
    , _btn_send_gcode(_("发送到机器(_S)"))
    , _btn_cancel_gcode(_("取消发送(_C)"))
    , _port_lbl(_("串口"))
    , _btn_refresh_ports(_("刷新端口"))
    , _chk_canvas_plot_preview(_("文档空间预览(_V)"))
    , _chk_machine_space_preview(_("机器空间预览(_P)（mm -> 画布）"))
    , _chk_send_from_cursor_line(_("仅从光标所在行向下发送(_C)"))
    , _chk_sync_page_to_bed(_("连接/同步时把页面改成机器行程（可恢复）"))
    , _btn_read_firmware(_("同步绘图机参数"))
    , _btn_read_radio_mode(_("读取模式"))
    , _btn_apply_radio_mode(_("应用无线模式"))
    , _chk_swap_xy(_("交换 X/Y"))
    , _chk_invert_x(_("反转 X"))
    , _chk_invert_y(_("反转 Y"))
    , _chk_flip_y(_("按页面高度镜像 Y"))
    , _chk_align_origin(_("左下角对齐到机器原点"))
    , _chk_clip_bed(_("限制在机器床面内"))
    , _chk_long_pen_up(_("长距离空走时高抬笔"))
    , _chk_near_connect(_("近距离自动连笔"))
    , _chk_sparse_sampling(_("排线抽稀"))
    , _chk_manual_pen_change_to_home(_("手动换笔时先回到原点"))
    , _chk_manual_pen_change_prompt(_("手动换笔时弹出确认提示"))
    , _chk_tool_change_point(_("换笔前先去换笔点"))
{
    _workers = std::make_unique<GrblPanelWorkers>();
    _link = std::make_unique<Inkscape::Axidraw::GrblLink>();
    build_ui();
}

GrblControlPanel::~GrblControlPanel()
{
    ensure_machine_status_poll(false);
    _gcode_cancel.store(true, std::memory_order_release);
    join_gcode_stream_thread();
    if (_workers) {
        _workers->request_stop();
    }
    {
        std::lock_guard const lk(_port_mutex);
        if (_link) {
            _link->close();
        }
    }
    if (_workers) {
        _workers->join_all();
    }
}

void GrblControlPanel::on_map()
{
    DialogBase::on_map();
    refresh_port_list();
    schedule_plot_feedback_refresh(true);
    if (_btn_connect.get_active()) {
        ensure_machine_status_poll(true);
    }
}

void GrblControlPanel::on_unmap()
{
    clear_plot_preview_overlay();
    _plot_feedback_refresh_timer.disconnect();
    _plot_feedback_refresh_preview_requested = false;
    _plot_feedback_refresh_dispatch_pending = false;
    ensure_machine_status_poll(false);
    DialogBase::on_unmap();
}

void GrblControlPanel::desktopReplaced()
{
    schedule_plot_feedback_refresh(true);
}

void GrblControlPanel::documentReplaced()
{
    clear_grbl_page_restore_state(_page_restore_state);
    update_page_restore_button();
    schedule_plot_feedback_refresh(true);
}

void GrblControlPanel::selectionChanged(Inkscape::Selection * /*selection*/)
{
    schedule_plot_feedback_refresh(true);
}

void GrblControlPanel::selectionModified(Inkscape::Selection * /*selection*/, guint /*flags*/)
{
    schedule_plot_feedback_refresh(true);
}

void GrblControlPanel::update_page_restore_button()
{
    _btn_restore_page_size.set_sensitive(
        get_grbl_page_restore_button_sensitive(grbl_page_restore_state_is_saved(_page_restore_state),
                                               has_active_gcode_stream()));
}

bool GrblControlPanel::get_configured_bed_size_mm(double &bed_width_mm, double &bed_height_mm) const
{
    bed_width_mm = _bed_width_spin.get_value();
    bed_height_mm = _bed_depth_spin.get_value();
    return bed_width_mm > 0.0 && bed_height_mm > 0.0;
}

bool GrblControlPanel::prepare_document_bed_action(SPDocument *&doc, Geom::Rect &bounds, double &bed_w_doc,
                                                   double &bed_h_doc, Glib::ustring &error,
                                                   Glib::ustring const &empty_message) const
{
    doc = getDocument();
    if (!doc) {
        error = _("没有活动文档。");
        return false;
    }
    double bed_width_mm = 0.0;
    double bed_height_mm = 0.0;
    if (!get_configured_bed_size_mm(bed_width_mm, bed_height_mm) ||
        !prepare_grbl_document_bed_action(doc, bed_width_mm, bed_height_mm, bounds, bed_w_doc, bed_h_doc, error,
                                          empty_message)) {
        if (error.empty()) {
            error = _("机器行程无效，请先同步或设置床面宽度/深度。");
        }
        return false;
    }
    return true;
}

void GrblControlPanel::update_mapping_control_sensitivity(bool const allow_interaction)
{
    auto const manual_mode = _tool_change_mode_combo.get_active_id() == k_tool_change_mode_manual;
    auto const m6_mode = _tool_change_mode_combo.get_active_id() == k_tool_change_mode_m6;
    auto const state = make_grbl_panel_mapping_sensitivity(
        allow_interaction, _chk_clip_bed.get_active(), _chk_long_pen_up.get_active(),
        _chk_near_connect.get_active(), _chk_sparse_sampling.get_active(), manual_mode, m6_mode,
        _chk_tool_change_point.get_active());
    _chk_swap_xy.set_sensitive(state.swap_xy);
    _chk_invert_x.set_sensitive(state.invert_x);
    _chk_invert_y.set_sensitive(state.invert_y);
    _chk_flip_y.set_sensitive(state.flip_y);
    _chk_align_origin.set_sensitive(state.align_origin);
    _chk_clip_bed.set_sensitive(state.clip_bed);
    _chk_long_pen_up.set_sensitive(state.long_pen_up);
    _chk_near_connect.set_sensitive(state.near_connect);
    _chk_sparse_sampling.set_sensitive(state.sparse_sampling);
    _tool_change_mode_combo.set_sensitive(state.tool_change_mode);
    _draw_feed_spin.set_sensitive(state.draw_feed);
    _travel_feed_spin.set_sensitive(state.travel_feed);
    _pen_up_delay_spin.set_sensitive(state.pen_up_delay);
    _pen_down_delay_spin.set_sensitive(state.pen_down_delay);
    _pen_up_cmd_entry.set_sensitive(state.pen_up_cmd);
    _pen_down_cmd_entry.set_sensitive(state.pen_down_cmd);
    _chk_manual_pen_change_to_home.set_sensitive(state.manual_pen_change_to_home);
    _chk_manual_pen_change_prompt.set_sensitive(state.manual_pen_change_prompt);
    _chk_tool_change_point.set_sensitive(state.tool_change_point);
    _bed_width_spin.set_sensitive(state.bed_width);
    _bed_depth_spin.set_sensitive(state.bed_depth);
    _long_pen_up_spin.set_sensitive(state.long_pen_up_height);
    _long_move_dist_spin.set_sensitive(state.long_move_dist);
    _near_connect_dist_spin.set_sensitive(state.near_connect_dist);
    _sparse_keep_every_spin.set_sensitive(state.sparse_keep_every);
    _tool_change_x_spin.set_sensitive(state.tool_change_x);
    _tool_change_y_spin.set_sensitive(state.tool_change_y);
}

GrblRuntimePhase GrblControlPanel::get_runtime_phase() const
{
    return get_runtime_state_view().phase;
}

GrblRuntimeStateView GrblControlPanel::get_runtime_state_view() const
{
    return make_grbl_runtime_state_view(
        _connecting.load(std::memory_order_acquire),
        _firmware_syncing.load(std::memory_order_acquire),
        _gcode_sending.load(std::memory_order_acquire),
        _gcode_cancel.load(std::memory_order_acquire));
}

bool GrblControlPanel::has_active_gcode_stream() const
{
    return get_runtime_state_view().gcode_active;
}

bool GrblControlPanel::is_runtime_busy() const
{
    return get_runtime_state_view().busy;
}

void GrblControlPanel::refresh_runtime_ui_state()
{
    auto const state = get_runtime_state_view();
    auto const action_presentation = make_grbl_action_buttons_presentation(state);
    auto const connection_presentation = make_grbl_connection_presentation(state, _link && _link->is_open());
    bool const allow_interaction = !state.busy;

    _btn_connect.set_sensitive(allow_interaction);
    _btn_read_firmware.set_sensitive(allow_interaction);
    _radio_mode_combo.set_sensitive(allow_interaction);
    _radio_pwd.set_sensitive(allow_interaction);
    _chk_radio_restart.set_sensitive(allow_interaction);
    _btn_read_radio_mode.set_sensitive(allow_interaction);
    _btn_apply_radio_mode.set_sensitive(allow_interaction);
    _btn_load_gcode.set_sensitive(allow_interaction);
    _btn_fill_from_drawing.set_sensitive(allow_interaction);
    _btn_send_from_drawing.set_sensitive(allow_interaction);
    _btn_save_gcode.set_sensitive(allow_interaction);
    _chk_send_from_cursor_line.set_sensitive(allow_interaction);
    _chk_sync_page_to_bed.set_sensitive(allow_interaction);
    _chk_canvas_plot_preview.set_sensitive(allow_interaction);
    _chk_machine_space_preview.set_sensitive(allow_interaction);
    update_mapping_control_sensitivity(allow_interaction);
    _start_gcode_view.set_sensitive(allow_interaction);
    _end_gcode_view.set_sensitive(allow_interaction);
    _gcode_view.set_sensitive(allow_interaction);
    _btn_send_gcode.set_sensitive(allow_interaction);
    _btn_cancel_gcode.set_sensitive(state.gcode_active && !state.cancel_requested);
    _btn_cancel_gcode.set_label(action_presentation.cancel_gcode.label);
    for (auto *b : {&_btn_mech_home, &_btn_yp, &_btn_set_origin, &_btn_xm, &_btn_goto_work_zero, &_btn_xp, &_btn_reset, &_btn_ym,
                    &_btn_pen_up, &_btn_pen_down, &_btn_motors, &_btn_clear_alarm, &_btn_fit_to_bed, &_btn_center_to_bed,
                    &_btn_restore_page_size}) {
        b->set_sensitive(allow_interaction);
    }
    _jog_dist.set_sensitive(allow_interaction);
    update_page_restore_button();
    _btn_send_gcode.set_label(action_presentation.send_gcode.label);
    _btn_send_gcode.set_tooltip_text(action_presentation.send_gcode.tooltip);
    _btn_send_from_drawing.set_label(action_presentation.send_from_drawing.label);
    _btn_send_from_drawing.set_tooltip_text(action_presentation.send_from_drawing.tooltip);
    _btn_read_firmware.set_label(action_presentation.read_firmware.label);
    _btn_read_firmware.set_tooltip_text(action_presentation.read_firmware.tooltip);
    _btn_cancel_gcode.set_tooltip_text(action_presentation.cancel_gcode.tooltip);

    _port_combo.set_sensitive(connection_presentation.serial_controls_sensitive);
    _btn_refresh_ports.set_sensitive(connection_presentation.serial_controls_sensitive);
    _btn_connect.set_label(connection_presentation.connect.label);
    _btn_connect.set_tooltip_text(connection_presentation.connect.tooltip);
}

bool GrblControlPanel::set_runtime_flag(std::atomic<bool> &flag, bool const active)
{
    bool const previous = flag.exchange(active, std::memory_order_acq_rel);
    if (previous == active) {
        return false;
    }
    refresh_runtime_ui_state();
    return true;
}

void GrblControlPanel::begin_connect_attempt_ui(Glib::ustring const &status)
{
    apply_connection_plan(make_connect_attempt_begin_plan(), &status);
}

void GrblControlPanel::set_connecting_state(bool const active)
{
    set_runtime_flag(_connecting, active);
}

void GrblControlPanel::set_firmware_syncing_state(bool const active)
{
    set_runtime_flag(_firmware_syncing, active);
}

bool GrblControlPanel::begin_firmware_sync()
{
    if (!set_runtime_flag(_firmware_syncing, true)) {
        return false;
    }
    apply_transport_plan(make_transport_plan_for_firmware_sync_start(_link != nullptr));
    return true;
}

void GrblControlPanel::complete_firmware_sync_ui(bool const resume_machine_status_poll,
                                                 bool const refresh_plot_feedback)
{
    set_firmware_syncing_state(false);
    apply_transport_plan(make_transport_plan_for_firmware_sync_complete(_link != nullptr, resume_machine_status_poll,
                                                                        is_connect_active()));
    if (refresh_plot_feedback) {
        schedule_plot_feedback_refresh(false);
    }
}

bool GrblControlPanel::launch_firmware_sync_worker()
{
    if (!begin_firmware_sync()) {
        return false;
    }
    if (!start_short_worker([this](GrblPanelWorkers::StopFlag const &stop) {
        GrblPanelFirmwareSyncContext context{
            .link = _link.get(),
            .with_locked_open_link = [this](std::atomic<bool> const &stop_flag, std::function<void()> work) {
                return with_locked_open_link(stop_flag, std::move(work));
            },
            .finish_sync_ui = [this] {
                Glib::signal_idle().connect_once(sigc::track_object([this] {
                    complete_firmware_sync_ui(true, true);
                }, *this));
            },
            .apply_snapshot_to_ui = [this](GrblFirmwareSnapshot const &snapshot) {
                return apply_firmware_snapshot_to_ui(snapshot);
            },
            .set_firmware_info_text = [this](Glib::ustring const &text) { set_firmware_info_text(text); },
            .save_mapping_preferences = [this](bool refresh_preview) { save_mapping_preferences_from_ui(refresh_preview); },
            .schedule_plot_feedback_refresh = [this](bool refresh_preview) { schedule_plot_feedback_refresh(refresh_preview); },
            .post_status = [this](Glib::ustring const &text, bool is_error) { post_status(text, is_error); },
        };
        GrblPanelFirmwareSync::run(context, stop);
    }, _("当前面板正在关闭，无法同步固件参数。"))) {
        complete_firmware_sync_ui(true, false);
        return false;
    }
    return true;
}

bool GrblControlPanel::request_firmware_sync(GrblFirmwareSyncRequestOrigin const origin)
{
    auto const plan = make_grbl_firmware_sync_request_plan(origin, is_connect_active(), get_runtime_phase());
    if (plan.keep_delayed_request) {
        queue_delayed_connect_firmware_sync();
    }
    if (!plan.start_now) {
        return false;
    }
    return launch_firmware_sync_worker();
}

void GrblControlPanel::queue_delayed_connect_firmware_sync()
{
    _delayed_firmware_sync.disconnect();
    _delayed_firmware_sync = Glib::signal_timeout().connect(sigc::track_object([this] {
        request_firmware_sync(GrblFirmwareSyncRequestOrigin::delayed_connect);
        _delayed_firmware_sync.disconnect();
        return false;
    }, *this), 1200);
}

void GrblControlPanel::request_canvas_redraw() const
{
    if (auto *desk = getDesktop()) {
        if (auto *canvas = desk->getCanvas()) {
            canvas->queue_draw();
        }
    }
}

void GrblControlPanel::post_status(Glib::ustring const &text, bool const is_error)
{
    Glib::signal_idle().connect_once(sigc::track_object([this, text, is_error] {
        _status.set_use_markup(is_error);
        if (is_error) {
            _status.set_markup("<span foreground=\"red\">" + Glib::Markup::escape_text(text) + "</span>");
        } else {
            _status.set_text(text);
        }
    }, *this));
}

bool GrblControlPanel::start_short_worker(std::function<void(std::atomic<bool> const &)> work,
                                          Glib::ustring const &shutdown_message)
{
    if (_workers && _workers->start(std::move(work))) {
        return true;
    }
    if (!shutdown_message.empty()) {
        post_status(shutdown_message, true);
    }
    return false;
}

bool GrblControlPanel::with_locked_open_link(std::atomic<bool> const &stop, std::function<void()> work,
                                             bool const check_machine_blocked, bool const serial_required)
{
    if (stop.load(std::memory_order_acquire)) {
        return false;
    }

    Glib::ustring blocked_reason;
    if (check_machine_blocked && is_machine_command_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return false;
    }

    std::lock_guard const guard(_port_mutex);
    if (stop.load(std::memory_order_acquire)) {
        return false;
    }
    if (check_machine_blocked && is_machine_command_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return false;
    }
    if (!(_link && _link->is_open())) {
        post_not_connected_status(serial_required);
        return false;
    }

    work();
    return !stop.load(std::memory_order_acquire);
}

Inkscape::Axidraw::GrblLink *GrblControlPanel::grbl_link() const noexcept
{
    return _link.get();
}

std::atomic<bool> const *GrblControlPanel::gcode_cancel_flag() const noexcept
{
    return &_gcode_cancel;
}

bool GrblControlPanel::is_connect_active() const
{
    return _btn_connect.get_active();
}

void GrblControlPanel::set_firmware_info_text(Glib::ustring const &text)
{
    if (auto const buf = _firmware_info_view.get_buffer()) {
        buf->set_text(text);
    }
}

void GrblControlPanel::set_mapping_sync_suspended(bool const suspended) noexcept
{
    _suspend_mapping_sync = suspended;
}

bool GrblControlPanel::should_sync_page_to_bed_on_firmware_read() const
{
    return _chk_sync_page_to_bed.get_active();
}

GrblFirmwareSyncApplyResult GrblControlPanel::apply_firmware_snapshot_to_ui(GrblFirmwareSnapshot const &snapshot)
{
    auto update_check_if_needed = [](Gtk::CheckButton &button, bool const value) {
        if (button.get_active() == value) {
            return false;
        }
        button.set_active(value);
        return true;
    };
    auto update_spin_if_needed = [](Gtk::SpinButton &spin, double const value, double const epsilon = 1e-6) {
        if (std::abs(spin.get_value() - value) <= epsilon) {
            return false;
        }
        spin.set_value(value);
        return true;
    };

    auto const mapping_update = make_grbl_firmware_mapping_update(snapshot);
    GrblFirmwareSyncApplyResult result;
    set_mapping_sync_suspended(true);
    if (mapping_update.has_invert_x) {
        result.changed = update_check_if_needed(_chk_invert_x, mapping_update.invert_x) || result.changed;
    }
    if (mapping_update.has_invert_y) {
        result.changed = update_check_if_needed(_chk_invert_y, mapping_update.invert_y) || result.changed;
    }
    if (mapping_update.has_bed_width) {
        result.changed = update_spin_if_needed(_bed_width_spin, mapping_update.bed_width) || result.changed;
    }
    if (mapping_update.has_bed_depth) {
        result.changed = update_spin_if_needed(_bed_depth_spin, mapping_update.bed_depth) || result.changed;
    }
    set_mapping_sync_suspended(false);

    if (should_sync_page_to_bed_on_firmware_read() && snapshot.has_x_travel && snapshot.has_y_travel) {
        if (auto *doc = getDocument()) {
            auto const page_sync = apply_grbl_sync_page_to_bed_action(
                doc, snapshot.x_travel_mm, snapshot.y_travel_mm, _page_restore_state);
            result.page_synced = page_sync.page_synced;
            result.unit_synced = page_sync.unit_synced;
            if (result.page_synced) {
                request_canvas_redraw();
                update_page_restore_button();
                schedule_plot_feedback_refresh(true);
            }
        }
    }

    return result;
}

void GrblControlPanel::post_machine_status(Glib::ustring const &text)
{
    Glib::signal_idle().connect_once(sigc::track_object([this, text] {
        _machine_status.set_text(text);
    }, *this));
}

void GrblControlPanel::disconnect_controller(bool const announce_status)
{
    set_connecting_state(false);
    apply_connection_plan(make_disconnect_controller_plan());
    {
        std::lock_guard const lk(_port_mutex);
        if (_link && _link->is_open()) {
            _link->close();
            if (announce_status) {
                post_status(_("控制器连接已关闭。"), false);
            }
        }
    }
    post_machine_status({});
}

void GrblControlPanel::finish_connect_attempt_failed_ui(Glib::ustring const &status, bool const clear_machine_status)
{
    finish_connect_attempt_ui(false, status, true, clear_machine_status);
}

void GrblControlPanel::finish_connect_attempt_ui(bool const keep_connect_active, Glib::ustring const &status,
                                                 bool const is_error, bool const clear_machine_status)
{
    set_connecting_state(false);
    auto plan = make_connect_attempt_failure_plan(_btn_connect.get_active(), clear_machine_status);
    if (keep_connect_active && plan.connect_button == GrblPanelConnectButtonAction::deactivate) {
        plan.connect_button = GrblPanelConnectButtonAction::none;
    }
    plan.status_is_error = is_error;
    apply_connection_plan(plan, &status);
}

void GrblControlPanel::post_connection_status(Glib::ustring const &device,
                                              Inkscape::Axidraw::GrblProbeResult const *const probe)
{
    auto const probe_response = probe ? Glib::ustring(probe->response_line) : Glib::ustring{};
    post_status(build_grbl_connection_status(device, probe_response), false);
    if (!probe_response.empty()) {
        post_machine_status(probe_response);
    }
}

void GrblControlPanel::post_not_connected_status(bool const serial_required)
{
    post_status(build_grbl_not_connected_status(serial_required), true);
}

void GrblControlPanel::finish_connect_attempt_succeeded_ui(Glib::ustring const &device,
                                                           Inkscape::Axidraw::GrblProbeResult const &probe)
{
    finalize_successful_connection_ui(device, probe);
}

void GrblControlPanel::finalize_successful_connection_ui(Glib::ustring const &device,
                                                         Inkscape::Axidraw::GrblProbeResult const &probe)
{
    set_connecting_state(false);
    auto const plan = make_connect_attempt_success_plan(_btn_connect.get_active());
    if (plan.disconnect_link) {
        disconnect_controller(false);
        return;
    }

    if (plan.cancel_delayed_firmware_sync) {
        _delayed_firmware_sync.disconnect();
    }
    if (plan.post_status) {
        post_connection_status(device, &probe);
    }
    if (plan.update_poll) {
        ensure_machine_status_poll(plan.poll_enabled);
    }
    if (plan.trigger_firmware_sync_now) {
        request_firmware_sync(GrblFirmwareSyncRequestOrigin::connect_success);
    }
    if (plan.refresh_plot_feedback) {
        schedule_plot_feedback_refresh(false);
    }
    if (plan.schedule_delayed_firmware_sync && !plan.trigger_firmware_sync_now) {
        queue_delayed_connect_firmware_sync();
    }
}

bool GrblControlPanel::get_busy_reason(bool const block_connecting, bool const block_firmware_sync,
                                       bool const block_gcode_sending, Glib::ustring &reason) const
{
    return grbl_runtime_is_blocked(get_runtime_state_view(), block_connecting, block_firmware_sync,
                                   block_gcode_sending, reason);
}

bool GrblControlPanel::is_machine_command_blocked(Glib::ustring &reason) const
{
    return get_busy_reason(true, true, true, reason);
}

bool GrblControlPanel::is_export_operation_blocked(Glib::ustring &reason) const
{
    auto const state = get_runtime_state_view();
    if (state.busy) {
        reason = grbl_busy_reason_for_phase(state.phase, GrblBusyReasonContext::export_action);
        return true;
    }
    return false;
}

void GrblControlPanel::apply_mapping_preferences_to_ui(GrblPanelMappingPrefs const &values)
{
    _chk_sync_page_to_bed.set_active(values.sync_page_to_bed);
    _chk_swap_xy.set_active(values.swap_xy);
    _chk_invert_x.set_active(values.invert_x);
    _chk_invert_y.set_active(values.invert_y);
    _chk_flip_y.set_active(values.flip_y);
    _chk_align_origin.set_active(values.align_origin);
    _chk_clip_bed.set_active(values.clip_bed);
    _chk_long_pen_up.set_active(values.long_pen_up);
    _chk_near_connect.set_active(values.near_connect);
    _chk_sparse_sampling.set_active(values.sparse_sampling);
    _tool_change_mode_combo.set_active_id(
        get_grbl_tool_change_mode_id(values.auto_pause_between_layers, values.manual_pen_change, values.tool_change_m6));
    _chk_manual_pen_change_to_home.set_active(values.manual_pen_change_to_home);
    _chk_manual_pen_change_prompt.set_active(values.manual_pen_change_prompt);
    _chk_tool_change_point.set_active(values.tool_change_point);
    _draw_feed_spin.set_value(values.draw_feed);
    _travel_feed_spin.set_value(values.travel_feed);
    _pen_up_delay_spin.set_value(values.pen_up_delay);
    _pen_down_delay_spin.set_value(values.pen_down_delay);
    _pen_up_cmd_entry.set_text(values.pen_up_cmd);
    _pen_down_cmd_entry.set_text(values.pen_down_cmd);
    _bed_width_spin.set_value(values.bed_width);
    _bed_depth_spin.set_value(values.bed_depth);
    _long_pen_up_spin.set_value(values.long_pen_up_height);
    _long_move_dist_spin.set_value(values.long_move_dist);
    _near_connect_dist_spin.set_value(values.near_connect_dist);
    _sparse_keep_every_spin.set_value(values.sparse_keep_every);
    _tool_change_x_spin.set_value(values.tool_change_x);
    _tool_change_y_spin.set_value(values.tool_change_y);
    if (auto const buf = _start_gcode_view.get_buffer()) {
        buf->set_text(values.start_gcode);
    }
    if (auto const buf = _end_gcode_view.get_buffer()) {
        buf->set_text(values.end_gcode);
    }
}

GrblPanelMappingPrefs GrblControlPanel::read_mapping_preferences_from_ui() const
{
    GrblPanelMappingPrefs values;
    values.sync_page_to_bed = _chk_sync_page_to_bed.get_active();
    values.swap_xy = _chk_swap_xy.get_active();
    values.invert_x = _chk_invert_x.get_active();
    values.invert_y = _chk_invert_y.get_active();
    values.flip_y = _chk_flip_y.get_active();
    values.align_origin = _chk_align_origin.get_active();
    values.clip_bed = _chk_clip_bed.get_active();
    values.long_pen_up = _chk_long_pen_up.get_active();
    values.near_connect = _chk_near_connect.get_active();
    values.sparse_sampling = _chk_sparse_sampling.get_active();
    values.manual_pen_change_to_home = _chk_manual_pen_change_to_home.get_active();
    values.manual_pen_change_prompt = _chk_manual_pen_change_prompt.get_active();
    values.tool_change_point = _chk_tool_change_point.get_active();
    values.draw_feed = _draw_feed_spin.get_value();
    values.travel_feed = _travel_feed_spin.get_value();
    values.pen_up_delay = _pen_up_delay_spin.get_value();
    values.pen_down_delay = _pen_down_delay_spin.get_value();
    values.pen_up_cmd = _pen_up_cmd_entry.get_text();
    values.pen_down_cmd = _pen_down_cmd_entry.get_text();
    values.bed_width = _bed_width_spin.get_value();
    values.bed_depth = _bed_depth_spin.get_value();
    values.long_pen_up_height = _long_pen_up_spin.get_value();
    values.long_move_dist = _long_move_dist_spin.get_value();
    values.near_connect_dist = _near_connect_dist_spin.get_value();
    values.sparse_keep_every = static_cast<int>(_sparse_keep_every_spin.get_value());
    values.tool_change_x = _tool_change_x_spin.get_value();
    values.tool_change_y = _tool_change_y_spin.get_value();
    apply_grbl_tool_change_mode_id(_tool_change_mode_combo.get_active_id(), values);
    if (auto const buf = _start_gcode_view.get_buffer()) {
        values.start_gcode = buf->get_text();
    }
    if (auto const buf = _end_gcode_view.get_buffer()) {
        values.end_gcode = buf->get_text();
    }
    return values;
}

void GrblControlPanel::load_mapping_preferences_to_ui()
{
    auto *prefs = Inkscape::Preferences::get();
    migrate_clip_to_machine_bed_default(prefs);
    auto const values = load_grbl_panel_mapping_prefs(*prefs);
    set_mapping_sync_suspended(true);
    apply_mapping_preferences_to_ui(values);
    update_tool_change_mode_ui();
    update_mapping_control_sensitivity(true);
    set_mapping_sync_suspended(false);
}

void GrblControlPanel::save_mapping_preferences_from_ui(bool const refresh_preview)
{
    if (_suspend_mapping_sync) {
        return;
    }
    auto *prefs = Inkscape::Preferences::get();
    auto const values = read_mapping_preferences_from_ui();
    save_grbl_panel_mapping_prefs(*prefs, values);
    update_tool_change_mode_ui();
    handle_mapping_preferences_changed(refresh_preview);
}

void GrblControlPanel::connect_mapping_preference_signals()
{
    auto const connect_refreshing = [this](auto &widget, auto signal_getter) {
        (widget.*signal_getter)().connect([this] { save_mapping_preferences_from_ui(true); });
    };
    auto const connect_non_refreshing = [this](auto &widget, auto signal_getter) {
        (widget.*signal_getter)().connect([this] { save_mapping_preferences_from_ui(false); });
    };

    connect_refreshing(_chk_sync_page_to_bed, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_swap_xy, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_invert_x, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_invert_y, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_flip_y, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_align_origin, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_clip_bed, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_long_pen_up, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_near_connect, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_sparse_sampling, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_manual_pen_change_to_home, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_manual_pen_change_prompt, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_chk_tool_change_point, &Gtk::CheckButton::signal_toggled);
    connect_refreshing(_draw_feed_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_travel_feed_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_pen_up_delay_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_pen_down_delay_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_pen_up_cmd_entry, &Gtk::Entry::signal_changed);
    connect_refreshing(_pen_down_cmd_entry, &Gtk::Entry::signal_changed);
    connect_refreshing(_bed_width_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_bed_depth_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_long_pen_up_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_long_move_dist_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_near_connect_dist_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_sparse_keep_every_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_tool_change_x_spin, &Gtk::SpinButton::signal_value_changed);
    connect_refreshing(_tool_change_y_spin, &Gtk::SpinButton::signal_value_changed);
    _tool_change_mode_combo.signal_changed().connect([this] { save_mapping_preferences_from_ui(true); });
    if (auto const buf = _start_gcode_view.get_buffer()) {
        connect_non_refreshing(*buf, &Gtk::TextBuffer::signal_changed);
    }
    if (auto const buf = _end_gcode_view.get_buffer()) {
        connect_non_refreshing(*buf, &Gtk::TextBuffer::signal_changed);
    }
}

void GrblControlPanel::update_tool_change_mode_ui()
{
    update_mapping_control_sensitivity(!get_runtime_state_view().gcode_active);
}

bool GrblControlPanel::is_plot_feedback_blocked() const
{
    return get_runtime_state_view().busy;
}

bool GrblControlPanel::require_active_plot_target(SPDocument *&doc, SPDesktop *&desktop, bool const clear_preview_on_failure)
{
    doc = getDocument();
    desktop = getDesktop();
    if (doc && desktop) {
        return true;
    }

    post_status(_("没有活动文档或桌面。"), true);
    if (clear_preview_on_failure) {
        clear_plot_preview_overlay();
    }
    return false;
}

GrblPanelExportSession GrblControlPanel::make_export_session(SPDesktop *desktop, std::atomic<bool> const *cancel) const
{
    auto *prefs = Inkscape::Preferences::get();
    auto const params = make_export_params_from_preferences();
    GrblPanelExportSources sources;
    sources.desktop = desktop;
    sources.selection = desktop ? desktop->getSelection() : nullptr;
    sources.use_current_layer_without_selection = prefs->getBool(k_pref_limit_layer, false);
    sources.cancel = cancel;
    return make_grbl_panel_export_session(params, sources);
}

bool GrblControlPanel::prepare_active_export_target(SPDocument *&doc, SPDesktop *&desktop,
                                                    GrblPanelExportSession &session,
                                                    bool const clear_preview_on_failure)
{
    if (!require_active_plot_target(doc, desktop, clear_preview_on_failure)) {
        return false;
    }
    session = make_export_session(desktop);
    return true;
}

void GrblControlPanel::refresh_plot_feedback_after_gcode_change()
{
    handle_editor_gcode_changed();
}

void GrblControlPanel::clear_editor_gcode_generation_state()
{
    _editor_gcode_generation_state = {};
    _editor_gcode_generation_state_warned_stale = false;
}

void GrblControlPanel::remember_editor_gcode_generation_state(Inkscape::Axidraw::GrblExportParams const &params)
{
    _editor_gcode_generation_state = make_editor_gcode_generation_state(
        params.clip_to_machine_bed, params.swap_xy, params.invert_x, params.invert_y,
        params.flip_y_canvas, params.align_content_min_to_origin,
        params.machine_bed_width_mm, params.machine_bed_depth_mm);
    _editor_gcode_generation_state_warned_stale = false;
}

bool GrblControlPanel::current_mapping_matches_editor_gcode_generation_state() const
{
    auto const inputs = make_editor_gcode_generation_inputs(
        _chk_clip_bed.get_active(), _chk_swap_xy.get_active(), _chk_invert_x.get_active(),
        _chk_invert_y.get_active(), _chk_flip_y.get_active(), _chk_align_origin.get_active(),
        _bed_width_spin.get_value(), _bed_depth_spin.get_value());
    return editor_gcode_generation_state_matches(_editor_gcode_generation_state, inputs);
}

void GrblControlPanel::update_editor_gcode_generation_state_status()
{
    if (!_editor_gcode_generation_state.valid) {
        _editor_gcode_generation_state_warned_stale = false;
        return;
    }

    if (current_mapping_matches_editor_gcode_generation_state()) {
        _editor_gcode_generation_state_warned_stale = false;
        return;
    }

    if (_editor_gcode_generation_state_warned_stale) {
        return;
    }

    _editor_gcode_generation_state_warned_stale = true;
    post_status(_("当前床面/映射设置已变更，编辑器中的图稿生成 G-code 已过期，请先重新“从图稿填充”。"), true);
}

void GrblControlPanel::handle_mapping_preferences_changed(bool const refresh_preview)
{
    auto const has_generated_state = _editor_gcode_generation_state.valid;
    auto const mapping_matches = !has_generated_state || current_mapping_matches_editor_gcode_generation_state();
    auto const plan = make_grbl_panel_mapping_change_plan(
        get_runtime_state_view().gcode_active, has_generated_state, mapping_matches,
        _editor_gcode_generation_state_warned_stale, refresh_preview);
    if (plan.warn_stale_generated_gcode) {
        post_status(build_editor_gcode_stale_mapping_message(), true);
    }
    if (!mapping_matches) {
        _editor_gcode_generation_state_warned_stale = plan.stale_warning_should_be_marked_posted ||
                                                      _editor_gcode_generation_state_warned_stale;
    } else {
        _editor_gcode_generation_state_warned_stale = false;
    }
    update_mapping_control_sensitivity(plan.allow_interaction);
    if (plan.refresh_plot_feedback) {
        refresh_plot_feedback(refresh_preview);
    }
}

void GrblControlPanel::handle_editor_gcode_changed(bool const generated_from_document)
{
    auto const plan = make_grbl_panel_editor_gcode_change_plan(
        generated_from_document, _suspend_editor_gcode_tracking, has_plot_preview_enabled());
    if (plan.clear_generation_state) {
        clear_editor_gcode_generation_state();
    }
    if (plan.reset_stale_warning_latch) {
        _editor_gcode_generation_state_warned_stale = false;
    }
    if (plan.schedule_plot_feedback_refresh) {
        schedule_plot_feedback_refresh(plan.refresh_preview);
    }
}

Gtk::Window *GrblControlPanel::get_dialog_parent_window(char const *missing_parent_message)
{
    auto *win = dynamic_cast<Gtk::Window *>(get_root());
    if (!win) {
        post_status(_(missing_parent_message), true);
    }
    return win;
}

GrblPanelSenderContext GrblControlPanel::make_sender_context()
{
    return {
        .link = _link.get(),
        .cancel = &_gcode_cancel,
        .post_status = [this](Glib::ustring const &text, bool is_error) { post_status(text, is_error); },
        .post_not_connected_status = [this] { post_not_connected_status(); },
        .post_gcode_stream_result = [this](std::string const &err) { post_gcode_stream_result(err); },
        .refresh_plot_feedback_after_gcode_change = [this] { refresh_plot_feedback_after_gcode_change(); },
        .finish_worker = [this](std::unique_lock<std::mutex> &lock) { finish_gcode_stream_worker(lock); },
        .with_plot_waits = [this](std::function<void()> work) { with_grbl_plot_waits(std::move(work)); },
    };
}

void GrblControlPanel::replace_editor_gcode_text(
    std::string const &text, std::optional<Inkscape::Axidraw::GrblExportParams> const generated_params)
{
    if (auto const buf = _gcode_view.get_buffer()) {
        _suspend_editor_gcode_tracking = true;
        buf->set_text(text);
        _suspend_editor_gcode_tracking = false;
    }

    if (generated_params) {
        remember_editor_gcode_generation_state(*generated_params);
    } else {
        clear_editor_gcode_generation_state();
    }

    handle_editor_gcode_changed(generated_params.has_value());
}

bool GrblControlPanel::get_editor_gcode_text(std::string &text, bool const send_from_cursor, guint *editor_line_1)
{
    auto const buf = _gcode_view.get_buffer();
    if (!buf) {
        text.clear();
        return false;
    }

    guint line_1 = 1;
    if (send_from_cursor) {
        Glib::RefPtr<Gtk::TextMark> const ins = buf->get_insert();
        Gtk::TextBuffer::iterator const it_mark = buf->get_iter_at_mark(ins);
        line_1 = static_cast<guint>(it_mark.get_line()) + 1;
        Gtk::TextBuffer::iterator const line0 = buf->get_iter_at_line(it_mark.get_line());
        text = buf->get_text(line0, buf->end(), false).raw();
    } else {
        text = buf->get_text().raw();
    }

    if (editor_line_1) {
        *editor_line_1 = line_1;
    }
    return true;
}

void GrblControlPanel::refresh_plot_summaries()
{
    auto *doc = getDocument();
    auto *desktop = getDesktop();
    if (!doc || !desktop) {
        _job_summary.set_markup(build_grbl_no_active_document_job_summary_markup());
        _layout_scale_summary.set_markup(build_grbl_no_active_document_scale_summary_markup());
        return;
    }

    auto const session = make_export_session(desktop);

    GrblPlotStats stats{};
    std::string err;
    if (!analyze_grbl_plot(doc, session.params, session.context, stats, err)) {
        _job_summary.set_markup(build_grbl_analysis_error_job_summary_markup(err));
        _layout_scale_summary.set_markup(build_grbl_analysis_error_scale_summary_markup(err));
        return;
    }

    auto const bed_width_mm = _bed_width_spin.get_value();
    auto const bed_height_mm = _bed_depth_spin.get_value();
    _job_summary.set_markup(build_grbl_job_summary_markup(stats, bed_width_mm, bed_height_mm));
    set_layout_scale_summary_from_stats(_layout_scale_summary, stats, bed_width_mm, bed_height_mm);
}

bool GrblControlPanel::has_plot_preview_enabled() const
{
    return _chk_canvas_plot_preview.get_active() || _chk_machine_space_preview.get_active();
}

void GrblControlPanel::schedule_plot_feedback_refresh(bool const refresh_preview)
{
    auto const plan = make_grbl_plot_feedback_schedule_plan(
        _plot_feedback_refresh_preview_requested, is_plot_feedback_blocked(), refresh_preview,
        _plot_feedback_refresh_dispatch_pending, _plot_feedback_refresh_timer.connected());
    _plot_feedback_refresh_preview_requested = plan.preview_requested;
    if (!plan.clear_preview_overlay && !plan.queue_idle_refresh) {
        return;
    }
    if (plan.clear_preview_overlay) {
        clear_plot_preview_overlay();
    }
    if (!plan.queue_idle_refresh) {
        return;
    }
    _plot_feedback_refresh_dispatch_pending = true;
    Glib::signal_idle().connect_once(sigc::track_object([this, refresh_preview] {
        _plot_feedback_refresh_dispatch_pending = false;
        refresh_plot_feedback(refresh_preview);
    }, *this));
}

void GrblControlPanel::refresh_plot_feedback(bool const refresh_preview)
{
    auto const plan = make_grbl_plot_feedback_refresh_plan(
        _plot_feedback_refresh_preview_requested, is_plot_feedback_blocked(), refresh_preview,
        _plot_feedback_refresh_timer.connected());
    _plot_feedback_refresh_preview_requested = plan.preview_requested;
    if (!plan.start_timer) {
        return;
    }
    _plot_feedback_refresh_timer = Glib::signal_timeout().connect(sigc::track_object([this] {
        auto const timer_plan = make_grbl_plot_feedback_timer_plan(_plot_feedback_refresh_preview_requested,
                                                                   has_plot_preview_enabled());
        _plot_feedback_refresh_preview_requested = timer_plan.preview_requested;
        _plot_feedback_refresh_timer.disconnect();
        if (timer_plan.refresh_summaries) {
            refresh_plot_summaries();
        }
        if (timer_plan.sync_preview_overlay) {
            sync_plot_preview_overlay();
        }
        return false;
    }, *this), 120);
}

bool GrblControlPanel::begin_gcode_stream_ui(GrblGcodeStartOrigin const origin)
{
    auto const plan = make_grbl_gcode_start_plan(get_runtime_state_view(), origin);
    if (!plan.allow_start) {
        if (plan.post_status) {
            post_status(plan.status, plan.status_is_error);
        }
        return false;
    }

    set_gcode_stream_ui_active(true);
    if (plan.post_status) {
        post_status(plan.status, plan.status_is_error);
    }
    return true;
}

void GrblControlPanel::post_gcode_stream_result(std::string const &err)
{
    auto const plan = make_grbl_gcode_result_plan(
        err == grbl_error_user_cancelled(),
        Glib::ustring(Inkscape::Axidraw::grbl_error_to_user_message(err)));
    if (plan.post_status) {
        post_status(plan.status, plan.status_is_error);
    }
}

void GrblControlPanel::join_gcode_stream_thread()
{
    std::lock_guard const lk(_gcode_stream_thread_mutex);
    if (_gcode_stream_thread.joinable()) {
        _gcode_stream_thread.join();
    }
}

void GrblControlPanel::start_gcode_stream_thread(std::function<void()> work)
{
    std::lock_guard const lk(_gcode_stream_thread_mutex);
    if (_gcode_stream_thread.joinable()) {
        _gcode_stream_thread.join();
    }
    _gcode_stream_thread = std::thread([work = std::move(work)]() mutable {
        work();
    });
}

void GrblControlPanel::run_gcode_stream_thread(std::function<void(std::unique_lock<std::mutex> &)> work)
{
    start_gcode_stream_thread([this, work = std::move(work)]() mutable {
        std::unique_lock<std::mutex> port_lock(_port_mutex);
        work(port_lock);
    });
}

void GrblControlPanel::finish_gcode_stream_worker(std::unique_lock<std::mutex> &port_lock)
{
    if (port_lock.owns_lock()) {
        port_lock.unlock();
    }
    finish_gcode_stream_from_worker();
}

void GrblControlPanel::with_grbl_plot_waits(std::function<void()> work)
{
    auto pump = [] {
        if (auto const ctx = Glib::MainContext::get_default()) {
            while (ctx->iteration(false)) {
            }
        }
    };
    grbl_begin_plot_waits(pump, &_gcode_cancel);
    scope_exit const end_plot{[] { grbl_end_plot_waits(); }};
    work();
}

void GrblControlPanel::ensure_machine_status_poll(bool const on)
{
    _machine_status_poll.disconnect();
    if (!on) {
        return;
    }
    _machine_status_poll = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &GrblControlPanel::on_machine_status_poll_timeout), 1500);
}

bool GrblControlPanel::on_machine_status_poll_timeout()
{
    if (!_btn_connect.get_active()) {
        return false;
    }
    auto const state = get_runtime_state_view();
    if (state.gcode_active || state.firmware_sync) {
        return true;
    }
    if (!_workers) {
        return false;
    }

    bool expected = false;
    if (!_machine_status_poll_in_flight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // Previous poll still running; keep timer, skip this tick.
        return true;
    }

    if (!start_short_worker([this](GrblPanelWorkers::StopFlag const &stop) {
            scope_exit const clear_in_flight{[this] {
                _machine_status_poll_in_flight.store(false, std::memory_order_release);
            }};
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            std::unique_lock<std::mutex> lk(_port_mutex, std::try_to_lock);
            if (!lk.owns_lock() || stop.load(std::memory_order_acquire)) {
                return;
            }
            if (!(_link && _link->is_open())) {
                return;
            }
            char const q = '?';
            Inkscape::Axidraw::grbl_debug_log_write("machine_status_poll", "?");
            if (!_link->write_bytes(&q, 1)) {
                return;
            }
            std::string line;
            if (!_link->read_line(line, 400) || stop.load(std::memory_order_acquire)) {
                return;
            }
            // If a prior command left an "ok" ahead of the report, read once more.
            if (line == "ok" && _link->read_line(line, 200)) {
                // use second line
            }
            if (!stop.load(std::memory_order_acquire)) {
                post_machine_status(Glib::ustring(line));
            }
        })) {
        _machine_status_poll_in_flight.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

void GrblControlPanel::refresh_port_list()
{
    _suspend_port_combo = true;
    auto *prefs = Inkscape::Preferences::get();
    Glib::ustring const cur_pref = prefs->getString(k_pref_device);
    Glib::ustring const net_host = prefs->getString(k_pref_net_host);
    int const net_port = prefs->getIntLimited(k_pref_net_port, 23, 1, 65535);
    std::vector<std::string> ports = Inkscape::Axidraw::enumerate_serial_ports();
    _port_combo.remove_all();

    bool seen_pref = false;
    for (auto const &p : ports) {
        Glib::ustring const u(p);
        _port_combo.append(u, u);
        if (u == cur_pref) {
            seen_pref = true;
        }
    }
    if (!cur_pref.empty() && !seen_pref) {
        _port_combo.append(cur_pref, cur_pref);
    }
    if (!net_host.empty()) {
        Glib::ustring const tcp_spec = "tcp://" + net_host + ":" + std::to_string(net_port);
        if (tcp_spec != cur_pref) {
            _port_combo.append(tcp_spec, tcp_spec);
        }
    }

    if (!cur_pref.empty()) {
        _port_combo.set_active_id(cur_pref);
    } else if (ports.empty()) {
        // leave empty
    } else {
        _port_combo.set_active(0);
    }
    _suspend_port_combo = false;
}

void GrblControlPanel::on_port_combo_changed()
{
    if (_suspend_port_combo) {
        return;
    }
    Glib::ustring id = _port_combo.get_active_id();
    if (id.empty()) {
        id = _port_combo.get_active_text();
    }
    if (id.empty()) {
        return;
    }
    auto *prefs = Inkscape::Preferences::get();
    prefs->setString(k_pref_device, id);
    prefs->save();
}

bool GrblControlPanel::resolve_connect_request(GrblPanelConnectRequest &request)
{
    auto *prefs = Inkscape::Preferences::get();
    request.device = prefs->getString(k_pref_device);
    if (request.device.empty()) {
        request.device = _port_combo.get_active_id();
        if (request.device.empty()) {
            request.device = _port_combo.get_active_text();
        }
        if (!request.device.empty()) {
            prefs->setString(k_pref_device, request.device);
            prefs->save();
        }
    }
    if (request.device.empty()) {
        _btn_connect.set_active(false);
        post_status(_("请先在上方选择串口，或在“首选项”的 GRBL 标签页中设置“串口设备”。"), true);
        return false;
    }

    request.baud = prefs->getIntLimited(k_pref_baud, 115200, 9600, 230400);
    parse_tcp_device_spec(request.device.raw(), request.tcp_host, request.tcp_port);
    return true;
}

void GrblControlPanel::start_connect_worker(GrblPanelConnectRequest request)
{
    begin_connect_attempt_ui(make_connect_probe_status(request.device, request.baud, request.use_tcp()));

    // Run open/probe on a worker to avoid blocking UI if driver stalls.
    if (!start_short_worker(
            [this, request = std::move(request)](GrblPanelWorkers::StopFlag const &stop) {
                auto result = GrblPanelConnectAttempt::run(request, stop);
                if (result.outcome == GrblPanelConnectAttemptOutcome::cancelled) {
                    return;
                }
                if (result.outcome == GrblPanelConnectAttemptOutcome::open_failed) {
                    Glib::signal_idle().connect_once(sigc::track_object(
                        [this,
                         use_tcp = request.use_tcp(),
                         timed_out = result.serial_open_timed_out,
                         access_denied = result.serial_open_access_denied] {
                            finish_connect_attempt_failed_ui(describe_connect_open_failure_ui(use_tcp, timed_out, access_denied));
                        },
                        *this));
                    return;
                }
                if (result.outcome == GrblPanelConnectAttemptOutcome::probe_failed) {
                    auto const probe = Inkscape::Axidraw::GrblProbeResult{false, result.probe_response_line};
                    Glib::signal_idle().connect_once(sigc::track_object([this, probe, request] {
                        auto const status = request.use_tcp()
                            ? describe_tcp_probe_failure_ui(request.device, probe)
                            : describe_probe_failure_ui(request.device, request.baud, probe);
                        finish_connect_attempt_failed_ui(status, true);
                    }, *this));
                    return;
                }

                std::lock_guard guard(_port_mutex);
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
                if (request.use_tcp()) {
                    _link->set_tcp(std::move(result.tcp));
                } else {
                    _link->set_serial(std::move(result.serial));
                }
                auto const probe = Inkscape::Axidraw::GrblProbeResult{true, result.probe_response_line};
                Glib::signal_idle().connect_once(sigc::track_object([this, device = request.device, probe] {
                    finish_connect_attempt_succeeded_ui(device, probe);
                }, *this));
            }, _("当前面板正在关闭，无法启动连接。"))) {
        set_connecting_state(false);
        _btn_connect.set_active(false);
    }
}

void GrblControlPanel::run_action(std::function<void(std::string &)> work, bool const report_ok)
{
    if (!start_short_worker([this, w = std::move(work), report_ok](GrblPanelWorkers::StopFlag const &stop) mutable {
            std::string err;
            if (!with_locked_open_link(stop, [&] { w(err); }, true)) {
                return;
            }
            if (err.empty()) {
                if (report_ok) {
                    post_status(_("操作完成。"), false);
                }
            } else {
                post_status(Glib::ustring(Inkscape::Axidraw::grbl_error_to_user_message(err)), true);
            }
        }, _("当前面板正在关闭，无法启动新的控制器命令。"))) {
    }
}

void GrblControlPanel::on_read_firmware_settings()
{
    request_firmware_sync(GrblFirmwareSyncRequestOrigin::manual);
}

void GrblControlPanel::connect_toggle()
{
    bool const want = _btn_connect.get_active();
    if (!want) {
        disconnect_controller(true);
        return;
    }

    GrblPanelConnectRequest request;
    if (!resolve_connect_request(request)) {
        return;
    }
    start_connect_worker(std::move(request));
}

double GrblControlPanel::jog_distance_mm() const
{
    auto const t = _jog_dist.get_active_text();
    return std::strtod(t.c_str(), nullptr);
}

double GrblControlPanel::travel_feed_mm_min() const
{
    auto *prefs = Inkscape::Preferences::get();
    return prefs->getDoubleLimited(k_pref_travel, 6000.0, 60.0, 20000.0);
}

void GrblControlPanel::jog_axis(char const axis, double const sign, double const dist_mm, double const feed)
{
    run_action(
        [this, axis, sign, dist_mm, feed](std::string &e) {
            if (dist_mm <= 0) {
                e = _("点动距离必须为正数");
                return;
            }
            double const d0 = dist_mm * sign;
            std::ostringstream dstr;
            dstr.setf(std::ios::fixed);
            dstr << std::setprecision(6) << d0;
            int const ifeed = static_cast<int>(feed + 0.5);
            if (!send_link_lines(*_link, {"G21", "G91"}, e)) {
                return;
            }
            {
                std::ostringstream m;
                m << "G1 " << axis << dstr.str() << " F" << ifeed;
                if (!_link->send_line_wait_ok(m.str(), e)) {
                    return;
                }
            }
            if (!send_link_lines(*_link, {"G90"}, e)) {
                return;
            }
        },
        false);
}

void GrblControlPanel::jog_x(double const sign)
{
    double const d = jog_distance_mm();
    double const f = travel_feed_mm_min();
    jog_axis('X', sign, d, f);
}

void GrblControlPanel::jog_y(double const sign)
{
    double const d = jog_distance_mm();
    double const f = travel_feed_mm_min();
    jog_axis('Y', sign, d, f);
}

void GrblControlPanel::soft_reset()
{
    run_action(
        [this](std::string &e) {
            const char c = 0x18;
            if (!_link->write_bytes(&c, 1)) {
                e = _("无法向串口写入软复位字节");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            _link->purge_io();
            post_status(
                _("软复位已发送。如果端口不再响应，请先断开再重新连接。"), false);
        },
        false);
}

void GrblControlPanel::send_pen_state(bool const up)
{
    run_action([this, up](std::string &e) {
        (void)this;
        auto *prefs = Inkscape::Preferences::get();
        Glib::ustring const pctl = prefs->getString(k_pref_pen_control, "z");
        if (pctl == "m3m5" || pctl == "M3M5") {
            if (!_link->send_line_wait_ok(up ? "M5" : "M3 S1000", e)) {
                return;
            }
        } else {
            Glib::ustring const cmd = up ? prefs->getString(k_pref_pen_up, "G1 Z0 F3000") : prefs->getString(k_pref_pen_down, "G1 Z5 F3000");
            if (cmd.empty()) {
                e = up ? _("抬笔命令（首选项中设置）为空") : _("落笔命令（首选项中设置）为空");
                return;
            }
            if (!_link->send_line_wait_ok(cmd.raw(), e)) {
                return;
            }
        }
    });
}

void GrblControlPanel::on_fit_document_to_bed()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }

    auto *doc = static_cast<SPDocument *>(nullptr);
    Geom::Rect bounds;
    double bed_w_doc = 0.0;
    double bed_h_doc = 0.0;
    Glib::ustring error;
    if (!prepare_document_bed_action(doc, bounds, bed_w_doc, bed_h_doc, error, _("当前文档没有可缩放的绘图内容。"))) {
        post_status(error, true);
        return;
    }

    auto const plan = make_grbl_fit_to_bed_plan(bounds, bed_w_doc, bed_h_doc);
    if (!plan.valid) {
        post_status(_("无法计算缩放比例。"), true);
        return;
    }

    if (plan.shrink_applied) {
        doc->getRoot()->scaleChildItemsRec(Geom::Scale(plan.scale), bounds.min(), false);
    }
    doc->getRoot()->translateChildItems(Geom::Translate(plan.translate_x, plan.translate_y));
    finalize_grbl_document_geometry_change(doc, GrblDocumentGeometryChange::fit_to_bed);
    schedule_plot_feedback_refresh(true);
    post_status(build_grbl_fit_to_bed_status(plan.scale, plan.shrink_applied), false);
}

void GrblControlPanel::on_center_document_to_bed()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }

    auto *doc = static_cast<SPDocument *>(nullptr);
    Geom::Rect bounds;
    double bed_w_doc = 0.0;
    double bed_h_doc = 0.0;
    Glib::ustring error;
    if (!prepare_document_bed_action(doc, bounds, bed_w_doc, bed_h_doc, error, _("当前文档没有可居中的绘图内容。"))) {
        post_status(error, true);
        return;
    }

    auto const plan = make_grbl_center_to_bed_plan(bounds, bed_w_doc, bed_h_doc);
    if (!plan.valid) {
        post_status(_("无法计算居中位置。"), true);
        return;
    }

    doc->getRoot()->translateChildItems(Geom::Translate(plan.translate_x, plan.translate_y));
    finalize_grbl_document_geometry_change(doc, GrblDocumentGeometryChange::center_to_bed);
    schedule_plot_feedback_refresh(true);

    post_status(_("已将图稿整体居中到机器行程范围内。"), false);
}

void GrblControlPanel::on_restore_page_size()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    if (!grbl_page_restore_state_is_saved(_page_restore_state)) {
        post_status(_("当前没有可恢复的原页面尺寸记录。"), true);
        return;
    }

    auto *doc = getDocument();
    if (!doc) {
        post_status(_("没有活动文档。"), true);
        return;
    }

    auto *page = get_grbl_target_page(doc);
    apply_grbl_document_and_page_size_px(doc, page, _page_restore_state.saved_doc_width_px,
                                         _page_restore_state.saved_doc_height_px,
                                         _page_restore_state.saved_page_width_px,
                                         _page_restore_state.saved_page_height_px);
    request_canvas_redraw();
    finalize_grbl_document_geometry_change(doc, GrblDocumentGeometryChange::restore_page);
    clear_grbl_page_restore_state(_page_restore_state);
    update_page_restore_button();
    schedule_plot_feedback_refresh(true);
    post_status(_("已恢复同步前的页面尺寸。"), false);
}

void GrblControlPanel::set_controls_sensitive_for_gcode_stream(bool const allow_interaction)
{
    (void)allow_interaction;
    refresh_runtime_ui_state();
}

void GrblControlPanel::apply_transport_plan(GrblPanelTransportPlan const &plan)
{
    if (_link) {
        switch (plan.activity) {
            case GrblPanelLinkActivityState::idle:
                _link->set_activity(Inkscape::Axidraw::GrblLink::Activity::idle);
                break;
            case GrblPanelLinkActivityState::firmware_sync:
                _link->set_activity(Inkscape::Axidraw::GrblLink::Activity::firmware_sync);
                break;
            case GrblPanelLinkActivityState::streaming:
                _link->set_activity(Inkscape::Axidraw::GrblLink::Activity::streaming);
                break;
            case GrblPanelLinkActivityState::unchanged:
            default:
                break;
        }
    }
    if (plan.update_poll) {
        ensure_machine_status_poll(plan.poll_enabled);
    }
}

void GrblControlPanel::apply_connection_plan(GrblPanelConnectionUiPlan const &plan,
                                             Glib::ustring const *const status)
{
    if (plan.set_connecting) {
        set_connecting_state(true);
    }
    if (plan.cancel_delayed_firmware_sync) {
        _delayed_firmware_sync.disconnect();
    }
    if (plan.update_poll) {
        ensure_machine_status_poll(plan.poll_enabled);
    }
    if (plan.connect_button == GrblPanelConnectButtonAction::deactivate && _btn_connect.get_active()) {
        _btn_connect.set_active(false);
    }
    if (plan.clear_machine_status) {
        post_machine_status({});
    }
    if (plan.post_status && status) {
        post_status(*status, plan.status_is_error);
    }
}

void GrblControlPanel::set_gcode_stream_ui_active(bool const active)
{
    bool const sending_changed = _gcode_sending.exchange(active, std::memory_order_acq_rel) != active;
    bool const cancel_changed = _gcode_cancel.exchange(false, std::memory_order_acq_rel);
    if (active) {
        apply_transport_plan(make_transport_plan_for_gcode_stream(true, _link != nullptr, is_connect_active(),
                                                                  _firmware_syncing.load(std::memory_order_acquire)));
        _delayed_firmware_sync.disconnect();
    } else {
        apply_transport_plan(make_transport_plan_for_gcode_stream(false, _link != nullptr, is_connect_active(),
                                                                  _firmware_syncing.load(std::memory_order_acquire)));
    }
    if (sending_changed || cancel_changed) {
        refresh_runtime_ui_state();
    }
}

bool GrblControlPanel::set_gcode_cancel_requested(bool const active)
{
    bool const previous = _gcode_cancel.exchange(active, std::memory_order_acq_rel);
    if (previous == active) {
        return false;
    }
    refresh_runtime_ui_state();
    return true;
}

void GrblControlPanel::complete_gcode_stream_ui(bool const join_worker_thread)
{
    auto const plan = make_grbl_gcode_finish_plan(join_worker_thread);
    if (plan.join_worker_thread) {
        join_gcode_stream_thread();
    }
    if (plan.deactivate_stream) {
        set_gcode_stream_ui_active(false);
    }
    if (plan.refresh_plot_feedback) {
        schedule_plot_feedback_refresh(false);
    }
}

void GrblControlPanel::finish_gcode_stream_ui()
{
    Glib::signal_idle().connect_once(sigc::track_object([this] {
        complete_gcode_stream_ui(false);
    }, *this));
}

void GrblControlPanel::finish_gcode_stream_from_worker()
{
    Glib::signal_idle().connect_once(sigc::track_object([this] {
        complete_gcode_stream_ui(true);
    }, *this));
}

bool GrblControlPanel::request_gcode_cancel_ui()
{
    auto const state = get_runtime_state_view();
    auto const plan = make_grbl_gcode_cancel_plan(state.gcode_active, state.cancel_requested);
    if (!plan.should_request) {
        return false;
    }
    if (!set_gcode_cancel_requested(true)) {
        return false;
    }
    if (plan.post_status) {
        post_status(plan.status, false);
    }
    return true;
}

void GrblControlPanel::on_cancel_gcode_stream()
{
    request_gcode_cancel_ui();
}

void GrblControlPanel::clear_plot_preview_overlay()
{
    _plot_preview_overlay.reset();
    _plot_preview_machine_overlay.reset();
    _plot_preview_machine_axis_overlay.reset();
    _plot_preview_axis_origin_label.reset();
    _plot_preview_axis_x_label.reset();
    _plot_preview_axis_y_label.reset();
    request_canvas_redraw();
}

bool GrblControlPanel::build_document_preview_overlay(SPDocument *doc, SPDesktop *desktop,
                                                      Inkscape::Axidraw::GrblExportParams const &params,
                                                      Inkscape::Axidraw::GrblExportContext const &ctx,
                                                      Geom::Affine const &affine, Glib::ustring &status_note)
{
    if (!_chk_canvas_plot_preview.get_active()) {
        return true;
    }

    Geom::PathVector pv_doc;
    std::string err;
    std::size_t included = 0;
    std::size_t total = 0;
    if (!build_grbl_plot_preview_pathvector(doc, params, ctx, pv_doc, err, k_preview_max_strokes, &included, &total)) {
        post_status(build_grbl_preview_build_error_status(err, false), true);
        return false;
    }
    if (pv_doc.empty()) {
        return true;
    }

    _plot_preview_overlay = make_canvasitem<CanvasItemBpath>(desktop->getCanvasTemp(),
                                                             transform_pathvector_to_desktop(pv_doc, affine), true);
    configure_preview_overlay(*_plot_preview_overlay, 0x22aaffcc, 1.0);
    status_note = build_grbl_preview_status_note(false, included, total, false);
    return true;
}

bool GrblControlPanel::build_machine_preview_overlay(SPDocument *doc, SPDesktop *desktop,
                                                     Inkscape::Axidraw::GrblExportParams const &params,
                                                     Inkscape::Axidraw::GrblExportContext const &ctx,
                                                     Geom::Affine const &affine, Glib::ustring &status_note)
{
    if (!_chk_machine_space_preview.get_active()) {
        return true;
    }

    Geom::PathVector pv_m;
    std::string err_m;
    bool clip_approx = false;
    std::size_t inc_m = 0;
    std::size_t tot_m = 0;
    if (!build_grbl_plot_machine_preview_pathvector_in_doc_space(doc, params, ctx, pv_m, err_m, &clip_approx, k_preview_max_strokes,
                                                                 &inc_m, &tot_m)) {
        post_status(build_grbl_preview_build_error_status(err_m, true), true);
        return false;
    }
    if (pv_m.empty()) {
        return true;
    }

    _plot_preview_machine_overlay = make_canvasitem<CanvasItemBpath>(desktop->getCanvasTemp(),
                                                                     transform_pathvector_to_desktop(pv_m, affine), true);
    configure_preview_overlay(*_plot_preview_machine_overlay, 0xff8844cc, 1.25);
    status_note = build_grbl_preview_status_note(true, inc_m, tot_m, clip_approx);
    return true;
}

void GrblControlPanel::build_machine_axis_overlay(SPDesktop *desktop, Inkscape::Axidraw::GrblExportParams const &params,
                                                  Geom::Affine const &affine)
{
    double const bed_w = std::max(1.0, _bed_width_spin.get_value());
    double const bed_h = std::max(1.0, _bed_depth_spin.get_value());
    Geom::Point const origin_dt = Geom::Point(0.0, 0.0) * affine;
    double const axis_len_doc = std::max(18.0, std::min(bed_w, bed_h) * 0.14);

    auto const x_dir_doc = machine_axis_direction(params.swap_xy, params.invert_x, params.invert_y, true);
    auto const y_dir_doc = machine_axis_direction(params.swap_xy, params.invert_x, params.invert_y, false);
    Geom::Point const x_end_dt = (Geom::Point(0.0, 0.0) + x_dir_doc * axis_len_doc) * affine;
    Geom::Point const y_end_dt = (Geom::Point(0.0, 0.0) + y_dir_doc * axis_len_doc) * affine;
    Geom::PathVector axis_pv;
    append_axis_arrow(axis_pv, origin_dt, x_end_dt, 12.0, 5.0);
    append_axis_arrow(axis_pv, origin_dt, y_end_dt, 12.0, 5.0);
    _plot_preview_machine_axis_overlay = make_canvasitem<CanvasItemBpath>(desktop->getCanvasTemp(), axis_pv, true);
    configure_preview_overlay(*_plot_preview_machine_axis_overlay, 0x00aa55ee, 2.0);

    _plot_preview_axis_origin_label = make_preview_axis_label(desktop, origin_dt + Geom::Point(8.0, -8.0), _("机器原点"),
                                                              0x003344dd);
    _plot_preview_axis_x_label = make_preview_axis_label(desktop, x_end_dt + Geom::Point(8.0, -8.0), _("机器 X+"),
                                                         0x005522dd);
    _plot_preview_axis_y_label = make_preview_axis_label(desktop, y_end_dt + Geom::Point(8.0, -8.0), _("机器 Y+"),
                                                         0x225500dd);
}

void GrblControlPanel::sync_plot_preview_overlay()
{
    clear_plot_preview_overlay();
    if (!_chk_canvas_plot_preview.get_active() && !_chk_machine_space_preview.get_active()) {
        return;
    }
    auto *desk = getDesktop();
    auto *doc = getDocument();
    if (!desk || !doc) {
        return;
    }
    auto const session = make_export_session(desk);

    Geom::Affine const aff = desk->doc2dt();
    Glib::ustring status_note;

    bool const doc_preview_ok = build_document_preview_overlay(doc, desk, session.params, session.context, aff, status_note);
    if (!doc_preview_ok && !_chk_machine_space_preview.get_active()) {
        return;
    }

    if (!build_machine_preview_overlay(doc, desk, session.params, session.context, aff, status_note)) {
        return;
    }
    auto const ui_plan = make_grbl_preview_overlay_ui_plan(_chk_machine_space_preview.get_active(), status_note);
    if (!ui_plan.build_machine_axis) {
        return;
    }

    build_machine_axis_overlay(desk, session.params, aff);
    if (ui_plan.post_status) {
        post_status(ui_plan.status, false);
    }
    if (ui_plan.request_canvas_redraw) {
        request_canvas_redraw();
    }
}

void GrblControlPanel::on_fill_gcode_from_document()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    SPDocument *doc = nullptr;
    SPDesktop *desktop = nullptr;
    GrblPanelExportSession session;
    if (!prepare_active_export_target(doc, desktop, session)) {
        return;
    }

    std::string out;
    std::string err;
    std::size_t strokes = 0;
    GrblPlotStats stats{};
    if (!build_grbl_plot_gcode_string(doc, session.params, session.context, out, err, &strokes, k_max_gcode_editor_bytes, &stats)) {
        clear_plot_preview_overlay();
        post_status(err.empty() ? Glib::ustring(_("无法根据当前图稿生成 G-code。")) : Glib::ustring(err), true);
        return;
    }
    replace_editor_gcode_text(out, session.params);
    post_status(build_grbl_fill_gcode_status(strokes, stats), false);
}

void GrblControlPanel::on_send_document_direct()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    SPDocument *doc = nullptr;
    SPDesktop *desktop = nullptr;
    GrblPanelExportSession session;
    if (!prepare_active_export_target(doc, desktop, session)) {
        return;
    }

    auto *win = dynamic_cast<Gtk::Window *>(get_root());
    GrblDirectSendGuardInput guard;
    guard.connection_state = (_link && _link->serial_port() && _link->serial_port()->is_open())
        ? GrblDirectSendConnectionState::ready_serial
        : ((_link && _link->kind() == Inkscape::Axidraw::GrblLink::Kind::tcp && _link->is_open())
               ? GrblDirectSendConnectionState::tcp_connected
               : GrblDirectSendConnectionState::not_connected);
    guard.requires_parent_window = session.params.manual_pen_change && session.params.pen_change_prompt;
    guard.has_parent_window = win != nullptr;
    auto const block_reason = get_grbl_direct_send_block_reason(guard);
    if (!block_reason.empty()) {
        post_status(block_reason, true);
        return;
    }

    bool const use_current_layer_without_selection = session.context.use_current_layer_without_selection;
    auto *selection = session.context.selection;
    refresh_plot_feedback_after_gcode_change();

    if (!begin_gcode_stream_ui(GrblGcodeStartOrigin::document_direct)) {
        return;
    }

    run_gcode_stream_thread([this, doc, desktop, selection, use_current_layer_without_selection, session, win]
                            (std::unique_lock<std::mutex> &port_lock) mutable {
        auto const context = make_sender_context();
        GrblPanelSender::run_direct_send_worker(context, port_lock, doc, desktop, selection,
                                               use_current_layer_without_selection, session.params, win);
    });
}

void GrblControlPanel::on_load_gcode_from_file()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    auto *win = get_dialog_parent_window("无法打开文件对话框（没有父窗口）。");
    if (!win) {
        return;
    }

    std::string folder;
    Inkscape::UI::Dialog::get_start_directory(folder, k_pref_save_gcode_dir, true);
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto gcode = Gtk::FileFilter::create();
    gcode->set_name(_("G-code 文件"));
    for (auto const *suffix : {"nc", "gcode", "tap", "cnc", "txt"}) {
        gcode->add_suffix(suffix);
    }
    filters->append(gcode);
    auto all = Gtk::FileFilter::create();
    all->set_name(_("所有文件"));
    all->add_pattern("*");
    filters->append(all);
    Glib::RefPtr<Gio::File> const src = choose_file_open(_("载入 G-code"), win, filters, folder, _("打开"));
    if (!src) {
        return;
    }
    std::string const path = src->get_path();
    if (path.empty()) {
        post_status(_("无法读取文件（没有本地路径）。"), true);
        return;
    }
    std::string contents;
    try {
        contents = Glib::file_get_contents(path);
    } catch (Glib::FileError const &e) {
        post_status(e.what(), true);
        return;
    }
    if (contents.size() > k_max_gcode_editor_bytes) {
        post_status(_("文件过大，无法载入编辑器。"), true);
        return;
    }
    replace_editor_gcode_text(contents);
    if (auto *prefs = Inkscape::Preferences::get()) {
        prefs->setString(k_pref_save_gcode_dir, folder);
    }
    post_status(build_grbl_gcode_load_status(src->get_parse_name()), false);
}

void GrblControlPanel::on_save_gcode_as()
{
    Glib::ustring blocked_reason;
    if (is_export_operation_blocked(blocked_reason)) {
        post_status(blocked_reason, true);
        return;
    }
    std::string text;
    if (!get_editor_gcode_text(text)) {
        return;
    }
    if (!grbl_gcode_text_has_content(text)) {
        post_status(_("没有可保存内容（G-code 为空）。"), true);
        return;
    }
    auto *win = get_dialog_parent_window("无法打开保存对话框（没有父窗口）。");
    if (!win) {
        return;
    }

    std::string folder;
    Inkscape::UI::Dialog::get_start_directory(folder, k_pref_save_gcode_dir, true);
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto gcode = Gtk::FileFilter::create();
    gcode->set_name(_("G-code 文件"));
    for (auto const *suffix : {"nc", "gcode", "tap", "cnc", "txt"}) {
        gcode->add_suffix(suffix);
    }
    filters->append(gcode);
    auto all = Gtk::FileFilter::create();
    all->set_name(_("所有文件"));
    all->add_pattern("*");
    filters->append(all);

    char const *document_filename = nullptr;
    if (auto *d = getDocument()) {
        document_filename = d->getDocumentFilename();
    }
    auto const initial = build_grbl_default_gcode_filename(document_filename);

    Glib::RefPtr<Gio::File> const dest = choose_file_save(_("G-code 另存为"), win, filters, initial, folder);
    if (!dest) {
        return;
    }
    std::string const path = dest->get_path();
    if (path.empty()) {
        post_status(_("无法确定要保存到的本地文件路径。"), true);
        return;
    }
    try {
        Glib::file_set_contents(path, text);
    } catch (Glib::FileError const &e) {
        post_status(e.what(), true);
        return;
    }
    if (auto *prefs = Inkscape::Preferences::get()) {
        prefs->setString(k_pref_save_gcode_dir, folder);
    }
    post_status(build_grbl_gcode_save_status(dest->get_parse_name()), false);
}

void GrblControlPanel::on_send_gcode()
{
    bool const send_from_cursor = _chk_send_from_cursor_line.get_active();
    guint editor_line_1 = 1;
    std::string text;
    if (!get_editor_gcode_text(text, send_from_cursor, &editor_line_1)) {
        return;
    }

    GrblEditorSendGuardInput guard;
    guard.send_from_cursor = send_from_cursor;
    guard.has_executable_content = std::any_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); });
    guard.stale_generated_gcode = _editor_gcode_generation_state.valid &&
                                  !current_mapping_matches_editor_gcode_generation_state();
    guard.check_bed_bounds = _chk_clip_bed.get_active();
    guard.bed_width_mm = _bed_width_spin.get_value();
    guard.bed_height_mm = _bed_depth_spin.get_value();
    if (guard.check_bed_bounds) {
        analyze_editor_gcode_bounds_mm(text, guard.bounds);
    }

    auto const block_reason = get_grbl_editor_send_block_reason(guard);
    if (!block_reason.empty()) {
        post_status(block_reason, true);
        return;
    }

    std::size_t const total_exec = count_executable_editor_gcode_lines(text, k_max_gcode_stream_lines);
    if (!begin_gcode_stream_ui(GrblGcodeStartOrigin::editor_gcode)) {
        return;
    }

    run_gcode_stream_thread(
        [this, text = std::move(text), total_exec, send_from_cursor, editor_line_1]
        (std::unique_lock<std::mutex> &port_lock) mutable
        {
            auto const context = make_sender_context();
            GrblPanelSender::run_editor_gcode_send_worker(context, port_lock, std::move(text), total_exec,
                                                          send_from_cursor, editor_line_1);
        });
}

void GrblControlPanel::build_ui()
{
    _jog_lbl.set_markup(_("<b>点动步长（mm）</b>"));
    for (char const *v : {"0.01", "0.1", "0.5", "1", "5", "10", "50", "100"}) {
        _jog_dist.append(v);
    }
    _jog_dist.set_active(3);

    _status.set_halign(Gtk::Align::START);
    _status.set_vexpand(false);
    _status.set_wrap(true);
    _status.set_max_width_chars(56);
    _status.set_selectable(true);
    _status.set_text(_("尚未连接。"));

    _btn_connect.set_label(_("连接"));
    _btn_connect.set_active(false);
    _btn_connect.set_tooltip_text(
        _("使用下方所选端口连接绘图机（也会保存到“编辑 -> 首选项 -> 输入/输出 -> 绘图机”）。"
          "波特率也来自同一页面。"));
    _radio_mode_combo.append("STA", _("WiFi 客户端（STA）"));
    _radio_mode_combo.append("AP", _("WiFi 热点（AP）"));
    _radio_mode_combo.append("BT", _("蓝牙（BT）"));
    _radio_mode_combo.append("OFF", _("关闭无线"));
    _radio_mode_combo.set_active_id("STA");
    _radio_mode_combo.set_hexpand(true);
    _radio_mode_combo.set_tooltip_text(
        _("通过 [ESP110] 设置 Grbl_ESP32 的无线模式。可选：STA / AP / BT / OFF。"));
    _radio_pwd.set_text("admin");
    _radio_pwd.set_visibility(false);
    _radio_pwd.set_placeholder_text(_("管理员密码"));
    _radio_pwd.set_hexpand(true);
    _radio_pwd.set_tooltip_text(
        _("[ESP110] 与可选的 [ESP444] 重启命令使用的管理员密码（默认通常是 admin）。"));
    _chk_radio_restart.set_label(_("切换模式后重启固件（[ESP444]）"));
    _chk_radio_restart.set_active(true);
    _chk_radio_restart.set_halign(Gtk::Align::START);
    _chk_radio_restart.set_tooltip_text(
        _("启用后，会在 [ESP110] 之后发送 [ESP444]RESTART，使无线模式立即生效。"));
    _btn_read_radio_mode.set_tooltip_text(
        _("通过 [ESP110]pwd=<password> 查询当前固件无线模式，并同步下拉框。"));
    _btn_apply_radio_mode.set_tooltip_text(
        _("向固件发送 [ESP110]<MODE>pwd=<password>。切换后可选触发 [ESP444]RESTART。"));

    _port_lbl.set_halign(Gtk::Align::START);
    _port_lbl.set_valign(Gtk::Align::CENTER);
    _port_lbl.set_markup(_("<b>Port</b>"));
    _port_combo.set_hexpand(true);
    _btn_refresh_ports.set_icon_name("view-refresh-symbolic");
    _btn_refresh_ports.set_tooltip_text(_("重新扫描串口"));
    _btn_refresh_ports.set_valign(Gtk::Align::CENTER);
    _btn_read_firmware.set_icon_name("document-properties-symbolic");
    _btn_read_firmware.set_tooltip_text(
        _("读取 $I / $G / $# / $$，并自动同步绘图机的方向反转与床面尺寸。"));
    _chk_sync_page_to_bed.set_halign(Gtk::Align::START);
    _chk_sync_page_to_bed.set_active(true);
    _chk_sync_page_to_bed.set_tooltip_text(
        _("启用后，如果固件返回了 $130/$131，就把当前文档页面尺寸同步成机器 X/Y 行程（单位 mm）。"));
    _machine_status.set_halign(Gtk::Align::START);
    _machine_status.set_ellipsize(Pango::EllipsizeMode::END);
    _machine_status.set_max_width_chars(56);
    _machine_status.add_css_class("monospace");
    _machine_status.set_tooltip_text(
        _("来自控制器的实时状态（连接期间大约每 1.5 秒轮询一次）。"));

    _frame.set_label(_("绘图机工作台"));
    _frame.set_margin_top(0);
    _frame.set_margin_bottom(0);
    _frame.set_margin_start(0);
    _frame.set_margin_end(0);

    _vbox.set_spacing(10);
    _vbox.set_margin_top(10);
    _vbox.set_margin_bottom(10);
    _vbox.set_margin_start(10);
    _vbox.set_margin_end(10);

    auto *grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(4);
    grid->set_column_spacing(4);
    grid->set_column_homogeneous(true);

    int r = 0;
    grid->attach(_btn_mech_home, 0, r, 1, 1);
    grid->attach(_btn_yp, 1, r, 1, 1);
    grid->attach(_btn_set_origin, 2, r, 1, 1);
    r++;
    grid->attach(_btn_xm, 0, r, 1, 1);
    grid->attach(_btn_goto_work_zero, 1, r, 1, 1);
    grid->attach(_btn_xp, 2, r, 1, 1);
    r++;
    grid->attach(_btn_reset, 0, r, 1, 1);
    grid->attach(_btn_ym, 1, r, 1, 1);
    r++;
    grid->attach(_jog_lbl, 0, r, 1, 1);
    grid->attach(_jog_dist, 1, r, 2, 1);
    r++;
    grid->attach(_btn_pen_up, 0, r, 1, 1);
    grid->attach(_btn_pen_down, 1, r, 1, 1);
    grid->attach(_btn_motors, 2, r, 1, 1);
    r++;
    grid->attach(_btn_clear_alarm, 0, r, 1, 1);

    for (auto *b : {&_btn_mech_home, &_btn_yp, &_btn_set_origin, &_btn_xm, &_btn_goto_work_zero, &_btn_xp, &_btn_reset, &_btn_ym,
                    &_btn_pen_up, &_btn_pen_down, &_btn_motors, &_btn_clear_alarm}) {
        b->set_hexpand(true);
    }
    _jog_dist.set_hexpand(true);

    _btn_mech_home.set_tooltip_text(_("回零：执行 $H（必须正确配置限位开关和安全间距）。"));
    _btn_yp.set_tooltip_text(_("Y 正向点动：先用相对模式 G1，再恢复为绝对模式 G90。"));
    _btn_set_origin.set_tooltip_text(_("G92：将当前位置设为工作零点（X0 Y0 Z0）。"));
    _btn_goto_work_zero.set_tooltip_text(_("G90 G0：以毫米单位（G21）快速移动到工作坐标 X0 Y0。"));
    _btn_goto_work_zero.set_icon_name("go-home-symbolic");
    _btn_xm.set_tooltip_text(_("按设定步长以毫米为单位向 X 负方向点动。"));
    _btn_xp.set_tooltip_text(_("按设定步长以毫米为单位向 X 正方向点动。"));
    _btn_ym.set_tooltip_text(_("按设定步长以毫米为单位向 Y 负方向点动。"));
    _btn_reset.set_tooltip_text(_("GRBL 软复位：发送 ASCII 0x18（Ctrl+X）。之后可能需要重新连接。"));
    _btn_reset.set_icon_name("view-refresh-symbolic");
    _btn_pen_up.set_tooltip_text(_("使用与绘图输出相同的抬笔方式（见 首选项 / GRBL）。"));
    _btn_pen_up.set_icon_name("go-up-symbolic");
    _btn_pen_down.set_tooltip_text(_("使用与绘图输出相同的落笔方式（见 首选项 / GRBL）。"));
    _btn_pen_down.set_icon_name("go-down-symbolic");
    _jog_dist.set_tooltip_text(_("X/Y 点动按钮使用的步长。"));
    _btn_fit_to_bed.set_tooltip_text(_("按当前机器床面宽/深，等比缩小整张图稿到行程内；如果本来就更小，则只移动到原点范围内。"));
    _btn_fit_to_bed.set_icon_name("transform-scale-symbolic");
    _btn_center_to_bed.set_tooltip_text(_("不改变图稿大小，只把整张图稿平移到当前机器行程的中心位置。"));
    _btn_center_to_bed.set_icon_name("align-horizontal-center-symbolic");
    _btn_restore_page_size.set_tooltip_text(_("如果页面曾被“连接/同步时把页面改成机器行程（可恢复）”改小，可用这里恢复到同步前的页面尺寸。"));
    _btn_restore_page_size.set_icon_name("edit-undo-symbolic");
    _layout_scale_summary.set_tooltip_text(_("这里会持续显示当前图稿相对机器行程的占用比例，以及“一键适配”后会缩放到多少。"));

    _chk_swap_xy.set_tooltip_text(_("将导出的机器坐标 X/Y 互换，适合机器坐标系相对画布旋转 90° 的情况。"));
    _chk_invert_x.set_tooltip_text(_("反转最终输出到机器的 X 坐标方向。"));
    _chk_invert_y.set_tooltip_text(_("反转最终输出到机器的 Y 坐标方向。"));
    _chk_flip_y.set_tooltip_text(_("按页面高度镜像 Y，用于把 SVG 画布的 Y 向下转换为机器常见的 Y 向上。"));
    _chk_align_origin.set_tooltip_text(_("将导出结果整体平移，使其左下角落在机器 X0 Y0。"));
    _chk_clip_bed.set_tooltip_text(_("将运动裁剪在床面范围内。超出部分会被截断。"));
    _chk_long_pen_up.set_tooltip_text(_("参考 kxnx 的绘图机策略：长距离空走前先抬到更高的位置，减少拖笔或蹭纸。"));
    _chk_near_connect.set_tooltip_text(_("参考 kxnx 的 nearDst 思路：如果相邻两段笔画距离很近，就直接连成一笔，减少抬笔和空走。启用后会实际画出这段连接线。"));
    _chk_sparse_sampling.set_tooltip_text(_("对规则排线或密集线场做快速抽稀：按笔画顺序每隔 N 条保留 1 条，减少发黑和绘制时间。"));
    _tool_change_mode_combo.append(k_tool_change_mode_none, _("不换笔"));
    _tool_change_mode_combo.append(k_tool_change_mode_manual, _("手动换笔"));
    _tool_change_mode_combo.append(k_tool_change_mode_m6, _("按图层工具号换笔(M6)"));
    _tool_change_mode_combo.set_active_id(k_tool_change_mode_none);
    _tool_change_mode_combo.set_tooltip_text(_("明确选择换笔模式：不换笔、手动换笔，或按图层名中的 T1/T2/T3 自动插入 Tn M6。"));
    _chk_manual_pen_change_to_home.set_tooltip_text(_("手动换笔前先回到 X0 Y0，便于取放画纸或人工换笔。"));
    _chk_manual_pen_change_prompt.set_tooltip_text(_("手动换笔时弹出确认提示，确认后再回到断点继续绘制。"));
    _chk_tool_change_point.set_tooltip_text(_("启用后，在发送 Tn M6 前先移动到固定换笔点。"));
    _draw_feed_spin.set_digits(0);
    _draw_feed_spin.set_range(60.0, 12000.0);
    _draw_feed_spin.set_increments(10.0, 100.0);
    _draw_feed_spin.set_tooltip_text(_("XY 绘制速度（G1，单位 mm/min）。"));
    _travel_feed_spin.set_digits(0);
    _travel_feed_spin.set_range(60.0, 20000.0);
    _travel_feed_spin.set_increments(10.0, 100.0);
    _travel_feed_spin.set_tooltip_text(_("XY 空走速度（G0/G1，单位 mm/min）。"));
    _pen_up_delay_spin.set_digits(0);
    _pen_up_delay_spin.set_range(0.0, 5000.0);
    _pen_up_delay_spin.set_increments(10.0, 100.0);
    _pen_up_delay_spin.set_tooltip_text(_("每次抬笔后等待这么多毫秒，再开始空走。适合抬笔机构反应偏慢。"));
    _pen_down_delay_spin.set_digits(0);
    _pen_down_delay_spin.set_range(0.0, 5000.0);
    _pen_down_delay_spin.set_increments(10.0, 100.0);
    _pen_down_delay_spin.set_tooltip_text(_("每次落笔后等待这么多毫秒，再开始画线。适合落笔后需要稳定时间。"));
    _pen_up_cmd_entry.set_placeholder_text(_("例如：G1 Z0 F3000"));
    _pen_up_cmd_entry.set_tooltip_text(_("抬笔命令。Z 速度也在这里改，例如把 F3000 改成更慢或更快。"));
    _pen_down_cmd_entry.set_placeholder_text(_("例如：G1 Z5 F3000"));
    _pen_down_cmd_entry.set_tooltip_text(_("落笔命令。Z 高度和 Z 速度都在这里改。"));
    _bed_width_spin.set_digits(2);
    _bed_width_spin.set_range(1.0, 2000.0);
    _bed_width_spin.set_increments(1.0, 10.0);
    _bed_width_spin.set_tooltip_text(_("床面宽度（机器 X 方向，单位 mm）。"));
    _bed_depth_spin.set_digits(2);
    _bed_depth_spin.set_range(1.0, 2000.0);
    _bed_depth_spin.set_increments(1.0, 10.0);
    _bed_depth_spin.set_tooltip_text(_("床面深度（机器 Y 方向，单位 mm）。"));
    _long_pen_up_spin.set_digits(2);
    _long_pen_up_spin.set_range(-1000.0, 1000.0);
    _long_pen_up_spin.set_increments(0.5, 5.0);
    _long_pen_up_spin.set_tooltip_text(_("长距离空走时使用的抬笔高度/位置。"));
    _long_move_dist_spin.set_digits(2);
    _long_move_dist_spin.set_range(0.0, 100000.0);
    _long_move_dist_spin.set_increments(1.0, 10.0);
    _long_move_dist_spin.set_tooltip_text(_("当两段笔画之间的空走距离达到这个值时，触发高抬笔。"));
    _near_connect_dist_spin.set_digits(2);
    _near_connect_dist_spin.set_range(0.0, 1000.0);
    _near_connect_dist_spin.set_increments(0.05, 0.5);
    _near_connect_dist_spin.set_tooltip_text(_("当相邻两段笔画的间距不大于这个值时，直接不断笔连过去。单位 mm。"));
    _sparse_keep_every_spin.set_digits(0);
    _sparse_keep_every_spin.set_range(1.0, 64.0);
    _sparse_keep_every_spin.set_increments(1.0, 5.0);
    _sparse_keep_every_spin.set_tooltip_text(_("抽稀步长。1 表示不过滤，2 表示隔 1 条留 1 条，3 表示每 3 条保留 1 条。"));
    _tool_change_x_spin.set_digits(2);
    _tool_change_y_spin.set_digits(2);
    _tool_change_x_spin.set_range(-2000.0, 2000.0);
    _tool_change_y_spin.set_range(-2000.0, 2000.0);
    _tool_change_x_spin.set_increments(1.0, 10.0);
    _tool_change_y_spin.set_increments(1.0, 10.0);
    _tool_change_x_spin.set_tooltip_text(_("换笔点 X 坐标（mm）。"));
    _tool_change_y_spin.set_tooltip_text(_("换笔点 Y 坐标（mm）。"));
    _firmware_info_view.set_editable(false);
    _firmware_info_view.set_cursor_visible(false);
    _firmware_info_view.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    _firmware_info_view.add_css_class("monospace");
    if (auto const buf = _firmware_info_view.get_buffer()) {
        buf->set_text(_("尚未读取固件参数。"));
    }
    _firmware_info_scroll.set_child(_firmware_info_view);
    _firmware_info_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _firmware_info_scroll.set_min_content_height(100);
    _firmware_info_scroll.set_has_frame(true);

    _gcode_frame.set_label(_("绘图任务"));
    _gcode_help.set_markup(
        _("<small><b>从图稿填充</b> 会按与“发送文档到绘图机”相同的规则生成任务（包括选择集、图层限制和绘图机首选项），"
          "这样你可以在发送前先在这里检查或编辑。"
          "<b>载入 G-code</b> 会用文件内容替换编辑器；<b>G-code 另存为</b> 会把编辑器内容写入文件。"
          "<b>机器空间预览</b> 是主预览：它会把最终的毫米路径（经过镜像、原点偏移，以及可选的床面裁剪）以橙色映射回画布。"
          "<b>文档空间预览</b> 是机器坐标映射之前的可选参考几何。"
          "<b>仅从光标所在行向下发送</b> 会跳过光标以上的内容（适合发生错误后的续传）。"
          "每行一条指令；以 <tt>;</tt> 开头的行和整行 <tt>(...)</tt> 注释在发送时会被跳过。"
          "每发送一行，都会等待设备返回 <tt>ok</tt>。发送过程中，日志会显示进度。"
          "<b>取消</b> 会在当前行结束后生效。</small>"));
    _gcode_help.set_wrap(true);
    _gcode_help.set_halign(Gtk::Align::START);
    _gcode_help.set_margin_bottom(4);
    _job_summary.set_halign(Gtk::Align::START);
    _job_summary.set_wrap(true);
    _job_summary.set_use_markup(true);
    _job_summary.set_selectable(true);
    _job_summary.set_xalign(0.0f);
    _job_summary.add_css_class("monospace");
    _job_summary.set_margin_top(4);
    _job_summary.set_margin_bottom(4);
    _job_summary.set_markup(_("<b>任务概览</b>\n尚未分析当前图稿。"));
    auto *job_summary_frame = Gtk::make_managed<Gtk::Frame>();
    job_summary_frame->set_label(_("任务概览"));
    job_summary_frame->set_child(_job_summary);
    auto *job_tuning_grid = Gtk::make_managed<Gtk::Grid>();
    job_tuning_grid->set_row_spacing(4);
    job_tuning_grid->set_column_spacing(8);
    job_tuning_grid->attach(_chk_near_connect, 0, 0, 1, 1);
    auto *lbl_near_connect = Gtk::make_managed<Gtk::Label>(_("连笔距离(mm)"), Gtk::Align::START);
    job_tuning_grid->attach(*lbl_near_connect, 1, 0, 1, 1);
    job_tuning_grid->attach(_near_connect_dist_spin, 2, 0, 1, 1);
    job_tuning_grid->attach(_chk_sparse_sampling, 0, 1, 1, 1);
    auto *lbl_sparse_sampling = Gtk::make_managed<Gtk::Label>(_("每隔 N 条留 1 条"), Gtk::Align::START);
    job_tuning_grid->attach(*lbl_sparse_sampling, 1, 1, 1, 1);
    job_tuning_grid->attach(_sparse_keep_every_spin, 2, 1, 1, 1);
    auto *lbl_tool_change_mode = Gtk::make_managed<Gtk::Label>(_("换笔模式"), Gtk::Align::START);
    job_tuning_grid->attach(*lbl_tool_change_mode, 0, 2, 1, 1);
    job_tuning_grid->attach(_tool_change_mode_combo, 1, 2, 2, 1);
    job_tuning_grid->attach(_chk_manual_pen_change_to_home, 0, 3, 2, 1);
    job_tuning_grid->attach(_chk_manual_pen_change_prompt, 0, 4, 2, 1);
    job_tuning_grid->attach(_chk_tool_change_point, 0, 5, 1, 1);
    auto *tool_change_xy_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto *lbl_tool_change_x = Gtk::make_managed<Gtk::Label>(_("X(mm)"), Gtk::Align::START);
    auto *lbl_tool_change_y = Gtk::make_managed<Gtk::Label>(_("Y(mm)"), Gtk::Align::START);
    tool_change_xy_box->append(*lbl_tool_change_x);
    tool_change_xy_box->append(_tool_change_x_spin);
    tool_change_xy_box->append(*lbl_tool_change_y);
    tool_change_xy_box->append(_tool_change_y_spin);
    job_tuning_grid->attach(*tool_change_xy_box, 1, 5, 2, 1);
    auto *job_tuning_hint = Gtk::make_managed<Gtk::Label>(
        _("<small>“近距离自动连笔”会把非常接近的相邻笔画合并成连续路径，减少抬笔和空走，但也会真的画出连接线。适合绘图机轮廓、描边类任务；如果不希望出现桥接线，请关闭。"
          "“排线抽稀”适合规则排线、阴影线、Sparse 图，按当前笔画顺序隔线保留，可显著减少发黑和总时长。"
          "换笔请明确选一种模式："
          "“手动换笔”会在分层切换时暂停，等你人工处理后再继续；"
          "“按图层工具号换笔(M6)”会读取图层名中的 T1/T2/T3，在层切换时插入 Tn M6，适合支持半自动换笔流程的固件。</small>"),
        Gtk::Align::START);
    job_tuning_hint->set_use_markup(true);
    job_tuning_hint->set_wrap(true);
    auto *start_gcode_lbl = Gtk::make_managed<Gtk::Label>(_("起始 G-code"), Gtk::Align::START);
    auto *end_gcode_lbl = Gtk::make_managed<Gtk::Label>(_("结束 G-code"), Gtk::Align::START);
    start_gcode_lbl->set_tooltip_text(_("在 G21/G90 之后、正式开始绘图之前插入的自定义指令。一行一条。"));
    end_gcode_lbl->set_tooltip_text(_("在最终抬笔之后插入的自定义指令。一行一条。"));
    _start_gcode_view.add_css_class("monospace");
    _end_gcode_view.add_css_class("monospace");
    _start_gcode_view.set_wrap_mode(Gtk::WrapMode::NONE);
    _end_gcode_view.set_wrap_mode(Gtk::WrapMode::NONE);
    _start_gcode_view.set_top_margin(4);
    _start_gcode_view.set_bottom_margin(4);
    _start_gcode_view.set_left_margin(4);
    _start_gcode_view.set_right_margin(4);
    _end_gcode_view.set_top_margin(4);
    _end_gcode_view.set_bottom_margin(4);
    _end_gcode_view.set_left_margin(4);
    _end_gcode_view.set_right_margin(4);
    _start_gcode_scroll.set_child(_start_gcode_view);
    _end_gcode_scroll.set_child(_end_gcode_view);
    _start_gcode_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _end_gcode_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _start_gcode_scroll.set_min_content_height(56);
    _end_gcode_scroll.set_min_content_height(56);
    _start_gcode_scroll.set_has_frame(true);
    _end_gcode_scroll.set_has_frame(true);
    _start_gcode_view.set_tooltip_text(_("例如：M117 Plot Start、G0 X0 Y0 等。一行一条；空行会忽略。"));
    _end_gcode_view.set_tooltip_text(_("例如：G0 X0 Y0、M84、M117 Plot Done 等。一行一条；空行会忽略。"));
    _chk_canvas_plot_preview.set_tooltip_text(
        _("可选的文档空间参考叠加层：使用与导出时相同的采样和笔画顺序，但发生在毫米换算、页面 Y 镜像、"
          "将绘图原点平移到 X0 Y0、以及机器床面裁剪之前。可用来对比原始几何与最终的机器空间预览。"));
    _chk_canvas_plot_preview.set_halign(Gtk::Align::START);
    _chk_machine_space_preview.set_tooltip_text(
        _("主预览叠加层：显示与最终 G-code 相同的折线路径，已完成毫米换算、可选页面 Y 镜像、"
          "将绘图原点平移到 X0 Y0，以及可选机器床面裁剪，然后再映射回文档单位。"
          "如果床面裁剪截掉了部分笔画，则这些区域的预览会是近似结果。"));
    _chk_machine_space_preview.set_halign(Gtk::Align::START);
    _chk_machine_space_preview.set_active(true);
    _chk_canvas_plot_preview.set_active(false);
    _chk_send_from_cursor_line.set_tooltip_text(
        _("启用后，“发送到机器”只会从文本光标所在行的开头一直发送到编辑器末尾。"
          "如果 Grbl 执行中报错，你可以删除或跳过已执行的行，然后把光标放到下一条命令处继续发送。"));
    _chk_send_from_cursor_line.set_halign(Gtk::Align::START);
    _gcode_view.set_accepts_tab(false);
    if (auto const buf = _gcode_view.get_buffer()) {
        buf->set_text("");
        buf->signal_changed().connect([this] {
            handle_editor_gcode_changed();
        });
    }
    _gcode_view.add_css_class("monospace");
    _gcode_view.set_top_margin(4);
    _gcode_view.set_bottom_margin(4);
    _gcode_view.set_left_margin(4);
    _gcode_view.set_right_margin(4);
    _gcode_view.set_vexpand(true);
    _gcode_scroll.set_child(_gcode_view);
    _gcode_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _gcode_scroll.set_vexpand(true);
    _gcode_scroll.set_min_content_height(120);
    _gcode_scroll.set_has_frame(true);

    _btn_cancel_gcode.set_sensitive(false);
    _btn_load_gcode.set_icon_name("document-open-symbolic");
    _btn_fill_from_drawing.set_icon_name("document-properties-symbolic");
    _btn_send_from_drawing.set_icon_name("media-playback-start-symbolic");
    _btn_save_gcode.set_icon_name("document-save-as-symbolic");
    _btn_send_gcode.set_icon_name("document-send-symbolic");
    _btn_cancel_gcode.set_icon_name("process-stop-symbolic");
    _btn_load_gcode.set_tooltip_text(_("从文本文件（UTF-8）替换编辑器内容，大小上限与生成任务相同。"));
    _btn_fill_from_drawing.set_tooltip_text(
        _("按当前绘图机首选项从当前文档生成 G-code（规则与主菜单中的“发送文档到绘图机”一致）。"));
    _btn_send_from_drawing.set_label(_("从图稿直接发送"));
    _btn_send_from_drawing.set_tooltip_text(
        _("按当前绘图机首选项直接从当前文档生成 G-code，并立刻发送到已连接的绘图机。"));
    _btn_save_gcode.set_tooltip_text(_("将编辑器中的文本保存为 .nc / .gcode 文件（UTF-8）。"));
    _btn_send_gcode.set_tooltip_text(
        _("按顺序发送每一条非空行，并在发送下一行前等待 Grbl 返回 ok（或错误）。"
          "长任务执行时，消息日志会更新大致的行数进度。"
          "也可以只从光标所在行开始发送（见上方复选框）。"));
    _btn_cancel_gcode.set_tooltip_text(
        _("设置取消标记；当前这一行仍可能执行完后才会停止发送。"));

    Inkscape::UI::pack_start(_gcode_inner, _gcode_help, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, *job_tuning_grid, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, *job_tuning_hint, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, *start_gcode_lbl, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _start_gcode_scroll, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, *end_gcode_lbl, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _end_gcode_scroll, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_canvas_plot_preview, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_machine_space_preview, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _chk_send_from_cursor_line, false, false, 2);
    Inkscape::UI::pack_start(_gcode_inner, _gcode_scroll, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_load_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_fill_from_drawing, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_send_from_drawing, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_save_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_send_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_actions, _btn_cancel_gcode, true, true, 2);
    Inkscape::UI::pack_start(_gcode_inner, _gcode_actions, false, false, 2);
    _gcode_frame.set_child(_gcode_inner);

    Inkscape::UI::pack_start(_port_row, _port_lbl, false, false, 6);
    Inkscape::UI::pack_start(_port_row, _port_combo, true, true, 6);
    Inkscape::UI::pack_start(_port_row, _btn_refresh_ports, false, false, 0);
    Inkscape::UI::pack_start(_port_row, _btn_read_firmware, false, false, 0);

    auto *frame_layout = Gtk::make_managed<Gtk::Frame>();
    frame_layout->set_label(_("页面、机器与恢复"));
    frame_layout->set_margin_top(0);
    auto *box_layout = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    auto *layout_hint = Gtk::make_managed<Gtk::Label>(
        _("<small>这里可以按机器行程整理图稿，也可以在页面被同步改小后恢复原页面尺寸；画布预览中会标出机器原点、机器 X+、机器 Y+ 方向。</small>"),
        Gtk::Align::START);
    layout_hint->set_use_markup(true);
    layout_hint->set_wrap(true);
    auto *top_actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    _btn_fit_to_bed.set_hexpand(true);
    _btn_center_to_bed.set_hexpand(true);
    _btn_restore_page_size.set_hexpand(true);
    top_actions->append(_btn_fit_to_bed);
    top_actions->append(_btn_center_to_bed);
    top_actions->append(_btn_restore_page_size);
    _layout_scale_summary.set_halign(Gtk::Align::START);
    _layout_scale_summary.set_wrap(true);
    _layout_scale_summary.set_use_markup(true);
    _layout_scale_summary.set_selectable(true);
    _layout_scale_summary.set_xalign(0.0f);
    _layout_scale_summary.add_css_class("monospace");
    _layout_scale_summary.set_margin_top(2);
    _layout_scale_summary.set_margin_bottom(2);
    _layout_scale_summary.set_markup(_("<b>当前缩放</b>\n尚未分析当前图稿与机器行程。"));
    update_page_restore_button();
    Inkscape::UI::pack_start(*box_layout, *top_actions, false, false, 0);
    Inkscape::UI::pack_start(*box_layout, _layout_scale_summary, false, false, 0);
    Inkscape::UI::pack_start(*box_layout, _chk_sync_page_to_bed, false, false, 0);
    Inkscape::UI::pack_start(*box_layout, *layout_hint, false, false, 0);
    frame_layout->set_child(*box_layout);

    auto *frame_serial = Gtk::make_managed<Gtk::Frame>();
    frame_serial->set_label(_("绘图机连接"));
    frame_serial->set_margin_top(0);
    auto *box_serial = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    Inkscape::UI::pack_start(*box_serial, _port_row, false, false, 0);
    auto *radio_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto *radio_lbl = Gtk::make_managed<Gtk::Label>(_("<b>无线与固件</b>"), Gtk::Align::START);
    radio_lbl->set_use_markup(true);
    Inkscape::UI::pack_start(*radio_row, *radio_lbl, false, false, 0);
    Inkscape::UI::pack_start(*radio_row, _radio_mode_combo, true, true, 0);
    Inkscape::UI::pack_start(*radio_row, _radio_pwd, true, true, 0);
    Inkscape::UI::pack_start(*radio_row, _btn_read_radio_mode, false, false, 0);
    Inkscape::UI::pack_start(*radio_row, _btn_apply_radio_mode, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, *radio_row, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _chk_radio_restart, false, false, 0);
    auto *hdr_status = Gtk::make_managed<Gtk::Label>();
    hdr_status->set_markup(_("<small>绘图机状态</small>"));
    hdr_status->set_halign(Gtk::Align::START);
    Inkscape::UI::pack_start(*box_serial, *hdr_status, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _machine_status, false, false, 0);
    Inkscape::UI::pack_start(*box_serial, _btn_connect, false, false, 0);
    frame_serial->set_child(*box_serial);

    auto *frame_mapping = Gtk::make_managed<Gtk::Frame>();
    frame_mapping->set_label(_("绘图范围与坐标映射"));
    auto *box_mapping = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto *mapping_grid = Gtk::make_managed<Gtk::Grid>();
    mapping_grid->set_row_spacing(4);
    mapping_grid->set_column_spacing(8);
    mapping_grid->attach(_chk_swap_xy, 0, 0, 1, 1);
    mapping_grid->attach(_chk_invert_x, 1, 0, 1, 1);
    mapping_grid->attach(_chk_invert_y, 2, 0, 1, 1);
    mapping_grid->attach(_chk_flip_y, 0, 1, 2, 1);
    mapping_grid->attach(_chk_align_origin, 2, 1, 1, 1);
    mapping_grid->attach(_chk_clip_bed, 0, 2, 1, 1);
    auto *lbl_bed_w = Gtk::make_managed<Gtk::Label>(_("床面宽(mm)"), Gtk::Align::START);
    auto *lbl_bed_d = Gtk::make_managed<Gtk::Label>(_("床面深(mm)"), Gtk::Align::START);
    mapping_grid->attach(*lbl_bed_w, 1, 2, 1, 1);
    mapping_grid->attach(_bed_width_spin, 2, 2, 1, 1);
    mapping_grid->attach(*lbl_bed_d, 1, 3, 1, 1);
    mapping_grid->attach(_bed_depth_spin, 2, 3, 1, 1);
    auto *lbl_draw_feed = Gtk::make_managed<Gtk::Label>(_("绘制速度(mm/min)"), Gtk::Align::START);
    auto *lbl_travel_feed = Gtk::make_managed<Gtk::Label>(_("空走速度(mm/min)"), Gtk::Align::START);
    auto *lbl_pen_up_delay = Gtk::make_managed<Gtk::Label>(_("抬笔后等待(ms)"), Gtk::Align::START);
    auto *lbl_pen_down_delay = Gtk::make_managed<Gtk::Label>(_("落笔后等待(ms)"), Gtk::Align::START);
    auto *lbl_pen_up_cmd = Gtk::make_managed<Gtk::Label>(_("抬笔 G-code"), Gtk::Align::START);
    auto *lbl_pen_down_cmd = Gtk::make_managed<Gtk::Label>(_("落笔 G-code"), Gtk::Align::START);
    mapping_grid->attach(*lbl_draw_feed, 0, 4, 1, 1);
    mapping_grid->attach(_draw_feed_spin, 1, 4, 2, 1);
    mapping_grid->attach(*lbl_travel_feed, 0, 5, 1, 1);
    mapping_grid->attach(_travel_feed_spin, 1, 5, 2, 1);
    mapping_grid->attach(*lbl_pen_up_delay, 0, 6, 1, 1);
    mapping_grid->attach(_pen_up_delay_spin, 1, 6, 2, 1);
    mapping_grid->attach(*lbl_pen_down_delay, 0, 7, 1, 1);
    mapping_grid->attach(_pen_down_delay_spin, 1, 7, 2, 1);
    mapping_grid->attach(*lbl_pen_up_cmd, 0, 8, 1, 1);
    mapping_grid->attach(_pen_up_cmd_entry, 1, 8, 2, 1);
    mapping_grid->attach(*lbl_pen_down_cmd, 0, 9, 1, 1);
    mapping_grid->attach(_pen_down_cmd_entry, 1, 9, 2, 1);
    mapping_grid->attach(_chk_long_pen_up, 0, 10, 1, 1);
    auto *lbl_long_pen = Gtk::make_managed<Gtk::Label>(_("高抬笔位置"), Gtk::Align::START);
    auto *lbl_long_move = Gtk::make_managed<Gtk::Label>(_("触发距离(mm)"), Gtk::Align::START);
    mapping_grid->attach(*lbl_long_pen, 1, 10, 1, 1);
    mapping_grid->attach(_long_pen_up_spin, 2, 10, 1, 1);
    mapping_grid->attach(*lbl_long_move, 1, 11, 1, 1);
    mapping_grid->attach(_long_move_dist_spin, 2, 11, 1, 1);
    auto *mapping_hint = Gtk::make_managed<Gtk::Label>(
        _("<small>“同步绘图机参数”会把 $3 同步到反转 X/Y，把 $130/$131 同步到床面尺寸。交换 X/Y 属于主机侧映射，需要你按绘图机结构手动设置。长距离高抬笔参考了 kxnx 绘图机软件中的做法；近距离连笔与起止 G-code 放在下方“绘图任务”区域统一设置。</small>"),
        Gtk::Align::START);
    mapping_hint->set_use_markup(true);
    mapping_hint->set_wrap(true);
    Inkscape::UI::pack_start(*box_mapping, *mapping_grid, false, false, 0);
    Inkscape::UI::pack_start(*box_mapping, *mapping_hint, false, false, 0);
    Inkscape::UI::pack_start(*box_mapping, _firmware_info_scroll, true, true, 0);
    frame_mapping->set_child(*box_mapping);

    auto *frame_motion = Gtk::make_managed<Gtk::Frame>();
    frame_motion->set_label(_("走笔与点动"));
    auto *box_motion = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    box_motion->append(*grid);
    frame_motion->set_child(*box_motion);

    auto *frame_log = Gtk::make_managed<Gtk::Frame>();
    frame_log->set_label(_("日志"));
    auto *box_log = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto *hdr_log = Gtk::make_managed<Gtk::Label>();
    hdr_log->set_markup(_("<small>消息</small>"));
    hdr_log->set_halign(Gtk::Align::START);
    Inkscape::UI::pack_start(*box_log, *hdr_log, false, false, 0);
    Inkscape::UI::pack_start(*box_log, _status, true, true, 0);
    frame_log->set_child(*box_log);

    Inkscape::UI::pack_start(_vbox, *frame_layout, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *job_summary_frame, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *frame_serial, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *frame_mapping, false, false, 0);
    Inkscape::UI::pack_start(_vbox, *frame_motion, false, false, 0);
    Inkscape::UI::pack_start(_vbox, _gcode_frame, true, true, 0);
    Inkscape::UI::pack_start(_vbox, *frame_log, false, false, 0);
    _frame.set_child(_vbox);
    append(_frame);

    load_mapping_preferences_to_ui();

    _btn_connect.signal_toggled().connect(sigc::mem_fun(*this, &GrblControlPanel::connect_toggle));
    _btn_refresh_ports.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::refresh_port_list));
    _btn_read_firmware.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_read_firmware_settings));
    _port_combo.signal_changed().connect(sigc::mem_fun(*this, &GrblControlPanel::on_port_combo_changed));
    connect_mapping_preference_signals();
    _btn_read_radio_mode.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            std::string const pwd = _radio_pwd.get_text();
            if (pwd.empty()) {
                e = _("管理员密码为空。");
                return;
            }
            std::string const cmd = "[ESP110]pwd=" + pwd + "\n";
            if (!_link->write_bytes(cmd.data(), cmd.size())) {
                e = _("无法发送无线模式查询命令。");
                return;
            }
            std::string reply;
            for (int i = 0; i < 8; ++i) {
                std::string line;
                if (!_link->read_line(line, 1200)) {
                    break;
                }
                trim_in_place(line);
                if (line.empty() || line == "ok") {
                    continue;
                }
                if (line.rfind("error", 0) == 0 || line.rfind("ERROR", 0) == 0) {
                    e = line;
                    return;
                }
                reply = line;
                break;
            }
            if (reply.empty()) {
                e = _("固件没有返回无线模式信息。");
                return;
            }
            std::string const mode = detect_radio_mode_from_reply(reply);
            if (mode.empty()) {
                post_status(
                    Glib::ustring::compose(_("已收到无线模式回复，但无法识别：%1"),
                                           Glib::ustring(reply)),
                    true);
                return;
            }
            Glib::signal_idle().connect_once(sigc::track_object([this, mode] {
                _radio_mode_combo.set_active_id(mode);
            }, *this));
            post_status(Glib::ustring::compose(_("当前固件无线模式：%1"), Glib::ustring(mode)), false);
        }, false);
    });
    _btn_apply_radio_mode.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            Glib::ustring mode = _radio_mode_combo.get_active_id();
            if (mode.empty()) {
                mode = "STA";
            }
            std::string const pwd = _radio_pwd.get_text();
            if (pwd.empty()) {
                e = _("管理员密码为空。");
                return;
            }
            std::string cmd = "[ESP110]" + mode.raw() + "pwd=" + pwd;
            if (!_link->send_line_wait_ok(cmd, e)) {
                return;
            }
            if (_chk_radio_restart.get_active()) {
                std::string restart_cmd = "[ESP444]RESTART pwd=" + pwd;
                std::string restart_err;
                if (!_link->send_line_wait_ok(restart_cmd, restart_err)) {
                    post_status(
                        Glib::ustring::compose(
                            _("无线模式命令已发送，但重启命令失败：%1。你可以手动重新连接。"),
                            Glib::ustring(restart_err)),
                        true);
                    return;
                }
            }
            Glib::ustring reconnect_hint;
            if (mode == "BT") {
                reconnect_hint = _("请将主机连接切换到蓝牙后重新连接。");
            } else if (mode == "AP") {
                reconnect_hint = _("请连接到控制器的 AP，然后使用其 AP IP 地址（通常是 192.168.0.1 或你配置的值）。");
            } else if (mode == "STA") {
                reconnect_hint = _("请使用控制器在 STA 模式下的 IP/主机名通过局域网重新连接。");
            } else {
                reconnect_hint = _("无线已关闭；请改用有线串口重新连接。");
            }
            post_status(
                Glib::ustring::compose(
                    _("无线模式命令已发送：%1。%2"),
                    mode, reconnect_hint),
                false);
        }, false);
    });

    _btn_mech_home.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!_link->send_line_wait_ok("$H", e)) {
                return;
            }
        });
    });
    _btn_yp.signal_clicked().connect([this] { jog_y(+1.0); });
    _btn_ym.signal_clicked().connect([this] { jog_y(-1.0); });
    _btn_xp.signal_clicked().connect([this] { jog_x(+1.0); });
    _btn_xm.signal_clicked().connect([this] { jog_x(-1.0); });
    _btn_set_origin.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!send_link_lines(*_link, {"G21", "G92 X0 Y0 Z0"}, e)) {
                return;
            }
        });
    });
    _btn_goto_work_zero.signal_clicked().connect([this] {
        run_action(
            [this](std::string &e) {
                if (!send_link_lines(*_link, {"G21", "G90", "G0 X0 Y0"}, e)) {
                    return;
                }
            });
    });
    _btn_reset.signal_clicked().connect([this] { soft_reset(); });
    _btn_pen_up.signal_clicked().connect([this] { send_pen_state(true); });
    _btn_pen_down.signal_clicked().connect([this] { send_pen_state(false); });
    _btn_motors.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!_link->send_line_wait_ok("$SLP", e)) {
                return;
            }
        });
    });
    _btn_clear_alarm.signal_clicked().connect([this] {
        run_action([this](std::string &e) {
            if (!_link->send_line_wait_ok("$X", e)) {
                return;
            }
        });
    });
    _chk_canvas_plot_preview.signal_toggled().connect([this] { schedule_plot_feedback_refresh(true); });
    _chk_machine_space_preview.signal_toggled().connect([this] { schedule_plot_feedback_refresh(true); });
    _chk_sync_page_to_bed.signal_toggled().connect([this] { save_mapping_preferences_from_ui(true); });
    _btn_fit_to_bed.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_fit_document_to_bed));
    _btn_center_to_bed.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_center_document_to_bed));
    _btn_restore_page_size.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_restore_page_size));
    _btn_load_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_load_gcode_from_file));
    _btn_fill_from_drawing.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_fill_gcode_from_document));
    _btn_send_from_drawing.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_send_document_direct));
    _btn_save_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_save_gcode_as));
    _btn_send_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_send_gcode));
    _btn_cancel_gcode.signal_clicked().connect(sigc::mem_fun(*this, &GrblControlPanel::on_cancel_gcode_stream));
}

} // namespace Inkscape::UI::Dialog

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
