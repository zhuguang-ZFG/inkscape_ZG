// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * @brief Serial GRBL / axis jog control panel (dockable / floating).
 */
#ifndef INKSCAPE_UI_GRBL_CONTROL_PANEL_H
#define INKSCAPE_UI_GRBL_CONTROL_PANEL_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/frame.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/entry.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/textview.h>
#include <gtkmm/togglebutton.h>

#include <sigc++/connection.h>

#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-ptr.h"
#include "ui/dialog/dialog-base.h"

namespace Inkscape::Axidraw {
class SerialPort;
class TcpPort;
} // namespace Inkscape::Axidraw

namespace Inkscape::UI::Dialog {

class GrblControlPanel final : public DialogBase
{
public:
    GrblControlPanel();
    ~GrblControlPanel() final;

private:
    void build_ui();
    void on_map() override;
    void on_unmap() override;
    void connect_toggle();
    void post_status(Glib::ustring const &text, bool is_error = false);
    void run_action(std::function<void(std::string &)> work, bool report_ok = true);
    void soft_reset();
    void jog_x(double sign);
    void jog_y(double sign);
    void jog_axis(char axis, double sign, double dist_mm, double feed);

    double jog_distance_mm() const;
    double travel_feed_mm_min() const;
    void send_pen_state(bool up);
    void on_send_gcode();
    void on_send_document_direct();
    void on_fill_gcode_from_document();
    void on_read_firmware_settings();
    void on_load_gcode_from_file();
    void on_save_gcode_as();
    void on_cancel_gcode_stream();
    void clear_plot_preview_overlay();
    void sync_plot_preview_overlay();
    void finish_gcode_stream_ui();
    void set_controls_sensitive_for_gcode_stream(bool allow);
    void update_connection_controls();

    void refresh_port_list();
    void maybe_auto_probe_and_connect();
    void on_port_combo_changed();
    void ensure_machine_status_poll(bool on);
    bool on_machine_status_poll_timeout();
    void post_machine_status(Glib::ustring const &text);
    void load_mapping_preferences_to_ui();
    void save_mapping_preferences_from_ui(bool refresh_preview = true);
    void update_tool_change_mode_ui();
    bool link_is_open() const;
    bool link_write_bytes(void const *data, size_t len);
    bool link_read_line(std::string &out, int timeout_ms);
    bool link_write_line(std::string const &line, std::string &err_out);
    void link_purge_io();
    void link_close();

    std::unique_ptr<Inkscape::Axidraw::SerialPort> _port;
    std::unique_ptr<Inkscape::Axidraw::TcpPort> _tcp_port;
    std::mutex _port_mutex;
    std::atomic<bool> _connecting{false};
    std::atomic<bool> _gcode_sending{false};
    std::atomic<bool> _gcode_cancel{false};

    sigc::connection _machine_status_poll;
    bool _suspend_port_combo{false};
    bool _auto_probe_attempted{false};
    bool _suspend_mapping_sync{false};

    Gtk::Frame _frame;
    Gtk::Box _vbox{Gtk::Orientation::VERTICAL};
    Gtk::Box _port_row{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Label _port_lbl;
    Gtk::ComboBoxText _port_combo;
    Gtk::Button _btn_refresh_ports;
    Gtk::CheckButton _chk_canvas_plot_preview;
    Gtk::CheckButton _chk_machine_space_preview;
    Gtk::CheckButton _chk_send_from_cursor_line;
    Gtk::Label _machine_status;
    Gtk::ToggleButton _btn_connect;
    Gtk::Button _btn_read_firmware;
    Gtk::ComboBoxText _radio_mode_combo;
    Gtk::Entry _radio_pwd;
    Gtk::CheckButton _chk_radio_restart;
    Gtk::Button _btn_read_radio_mode;
    Gtk::Button _btn_apply_radio_mode;
    Gtk::Label _status;
    Gtk::Label _jog_lbl;
    Gtk::ComboBoxText _jog_dist;

    Gtk::Button _btn_mech_home;
    Gtk::Button _btn_yp;
    Gtk::Button _btn_set_origin;
    Gtk::Button _btn_xm;
    Gtk::Button _btn_goto_work_zero;
    Gtk::Button _btn_xp;
    Gtk::Button _btn_reset;
    Gtk::Button _btn_ym;
    Gtk::Button _btn_pen_up;
    Gtk::Button _btn_pen_down;
    Gtk::Button _btn_motors;
    Gtk::Button _btn_clear_alarm;
    Gtk::CheckButton _chk_swap_xy;
    Gtk::CheckButton _chk_invert_x;
    Gtk::CheckButton _chk_invert_y;
    Gtk::CheckButton _chk_flip_y;
    Gtk::CheckButton _chk_align_origin;
    Gtk::CheckButton _chk_clip_bed;
    Gtk::CheckButton _chk_long_pen_up;
    Gtk::CheckButton _chk_near_connect;
    Gtk::CheckButton _chk_sparse_sampling;
    Gtk::ComboBoxText _tool_change_mode_combo;
    Gtk::CheckButton _chk_manual_pen_change_to_home;
    Gtk::CheckButton _chk_manual_pen_change_prompt;
    Gtk::CheckButton _chk_tool_change_point;
    Gtk::SpinButton _bed_width_spin;
    Gtk::SpinButton _bed_depth_spin;
    Gtk::SpinButton _long_pen_up_spin;
    Gtk::SpinButton _long_move_dist_spin;
    Gtk::SpinButton _near_connect_dist_spin;
    Gtk::SpinButton _sparse_keep_every_spin;
    Gtk::SpinButton _tool_change_x_spin;
    Gtk::SpinButton _tool_change_y_spin;
    Gtk::TextView _firmware_info_view;
    Gtk::ScrolledWindow _firmware_info_scroll;

    Gtk::Frame _gcode_frame;
    Gtk::Box _gcode_inner{Gtk::Orientation::VERTICAL};
    Gtk::Label _gcode_help;
    Gtk::TextView _start_gcode_view;
    Gtk::TextView _end_gcode_view;
    Gtk::ScrolledWindow _start_gcode_scroll;
    Gtk::ScrolledWindow _end_gcode_scroll;
    Gtk::ScrolledWindow _gcode_scroll;
    Gtk::TextView _gcode_view;
    Gtk::Box _gcode_actions{Gtk::Orientation::HORIZONTAL};
    Gtk::Button _btn_load_gcode;
    Gtk::Button _btn_fill_from_drawing;
    Gtk::Button _btn_send_from_drawing;
    Gtk::Button _btn_save_gcode;
    Gtk::Button _btn_send_gcode;
    Gtk::Button _btn_cancel_gcode;

    CanvasItemPtr<Inkscape::CanvasItemBpath> _plot_preview_overlay;
    CanvasItemPtr<Inkscape::CanvasItemBpath> _plot_preview_machine_overlay;
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_GRBL_CONTROL_PANEL_H
