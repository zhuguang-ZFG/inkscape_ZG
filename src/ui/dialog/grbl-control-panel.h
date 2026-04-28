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
#include <optional>
#include <thread>

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
#include "display/control/canvas-item-text.h"
#include "ui/dialog/grbl-editor-generation-state.h"
#include "ui/dialog/grbl-panel-connect-attempt.h"
#include "ui/dialog/grbl-panel-connection-state.h"
#include "ui/dialog/grbl-panel-document-geometry.h"
#include "ui/dialog/grbl-panel-export-session.h"
#include "ui/dialog/grbl-panel-feedback-trigger-state.h"
#include "ui/dialog/grbl-panel-firmware-sync-state.h"
#include "ui/dialog/grbl-panel-job-guard.h"
#include "ui/dialog/grbl-work-origin.h"
#include "ui/dialog/grbl-panel-mapping-prefs.h"
#include "ui/dialog/grbl-panel-presentation.h"
#include "ui/dialog/grbl-panel-sender.h"
#include "ui/dialog/grbl-panel-transport-state.h"
#include "ui/dialog/grbl-runtime-state.h"
#include "ui/dialog/grbl-panel-firmware-sync.h"
#include "ui/dialog/dialog-base.h"

namespace Inkscape::Axidraw {
class GrblLink;
struct GrblProbeResult;
struct GrblExportParams;
struct GrblExportContext;
} // namespace Inkscape::Axidraw

class SPPage;
class SPDocument;

namespace Inkscape::UI::Dialog {

class GrblPanelWorkers;
class GrblPanelSender;

class GrblControlPanel final : public DialogBase
{
public:
    GrblControlPanel();
    ~GrblControlPanel() final;

private:
    void build_ui();
    void on_map() override;
    void on_unmap() override;
    void desktopReplaced() override;
    void documentReplaced() override;
    void selectionChanged(Inkscape::Selection *selection) override;
    void selectionModified(Inkscape::Selection *selection, guint flags) override;
    void connect_toggle();
    void post_status(Glib::ustring const &text, bool is_error = false);
    bool start_short_worker(std::function<void(std::atomic<bool> const &)> work,
                            Glib::ustring const &shutdown_message = {});
    bool with_locked_open_link(std::atomic<bool> const &stop, std::function<void()> work,
                               bool check_machine_blocked = false, bool serial_required = false);
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
    void on_fit_document_to_bed();
    void on_center_document_to_bed();
    void on_restore_page_size();
    void on_fill_gcode_from_document();
    void on_read_firmware_settings();
    void on_load_gcode_from_file();
    void on_save_gcode_as();
    void on_cancel_gcode_stream();
    bool confirm_cancel_gcode_stream(bool &return_to_origin);
    bool request_gcode_cancel_ui(bool return_to_origin_after_cancel);
    void return_to_work_origin_after_cancel();
    void execute_work_origin_action(GrblWorkOriginAction action);
    void clear_plot_preview_overlay();
    void sync_plot_preview_overlay();
    void set_gcode_stream_ui_active(bool active);
    bool set_gcode_cancel_requested(bool active);
    void set_connecting_state(bool active);
    void set_firmware_syncing_state(bool active);
    void update_mapping_control_sensitivity(bool allow_interaction = true);
    bool is_runtime_busy() const;
    bool has_active_gcode_stream() const;
    GrblRuntimePhase get_runtime_phase() const;
    GrblRuntimeStateView get_runtime_state_view() const;
    void refresh_runtime_ui_state();
    bool set_runtime_flag(std::atomic<bool> &flag, bool active);
    void begin_connect_attempt_ui(Glib::ustring const &status);
    bool begin_firmware_sync();
    bool launch_firmware_sync_worker();
    bool request_firmware_sync(GrblFirmwareSyncRequestOrigin origin);
    void queue_delayed_connect_firmware_sync();
    void complete_firmware_sync_ui(bool resume_machine_status_poll, bool refresh_plot_feedback);
    void finish_gcode_stream_ui();
    /// Rejoins the G-code stream worker on the main loop, then mirrors @ref finish_gcode_stream_ui.
    void finish_gcode_stream_from_worker();
    void complete_gcode_stream_ui(bool join_worker_thread);
    void set_controls_sensitive_for_gcode_stream(bool allow);
    void apply_connection_plan(GrblPanelConnectionUiPlan const &plan, Glib::ustring const *status = nullptr);
    void apply_transport_plan(GrblPanelTransportPlan const &plan);
    bool get_busy_reason(bool block_connecting, bool block_firmware_sync, bool block_gcode_sending,
                         Glib::ustring &reason) const;
    bool is_machine_command_blocked(Glib::ustring &reason) const;
    bool is_export_operation_blocked(Glib::ustring &reason) const;
    Inkscape::Axidraw::GrblLink *grbl_link() const noexcept;
    std::atomic<bool> const *gcode_cancel_flag() const noexcept;
    bool is_connect_active() const;
    void set_firmware_info_text(Glib::ustring const &text);
    void set_mapping_sync_suspended(bool suspended) noexcept;
    bool should_sync_page_to_bed_on_firmware_read() const;
    GrblFirmwareSyncApplyResult apply_firmware_snapshot_to_ui(GrblFirmwareSnapshot const &snapshot);

    void refresh_port_list();
    void on_port_combo_changed();
    bool resolve_connect_request(GrblPanelConnectRequest &request);
    void start_connect_worker(GrblPanelConnectRequest request);
    void ensure_machine_status_poll(bool on);
    bool on_machine_status_poll_timeout();
    void post_machine_status(Glib::ustring const &text);
    void disconnect_controller(bool announce_status);
    void finish_connect_attempt_failed_ui(Glib::ustring const &status, bool clear_machine_status = false);
    void finish_connect_attempt_ui(bool keep_connect_active, Glib::ustring const &status, bool is_error,
                                   bool clear_machine_status = false);
    void finish_connect_attempt_succeeded_ui(Glib::ustring const &device,
                                             Inkscape::Axidraw::GrblProbeResult const &probe);
    void finalize_successful_connection_ui(Glib::ustring const &device, Inkscape::Axidraw::GrblProbeResult const &probe);
    void post_connection_status(Glib::ustring const &device, Inkscape::Axidraw::GrblProbeResult const *probe = nullptr);
    void post_not_connected_status(bool serial_required = false);
    void apply_mapping_preferences_to_ui(GrblPanelMappingPrefs const &values);
    GrblPanelMappingPrefs read_mapping_preferences_from_ui() const;
    void load_mapping_preferences_to_ui();
    void save_mapping_preferences_from_ui(bool refresh_preview = true);
    void connect_mapping_preference_signals();
    void update_tool_change_mode_ui();
    void refresh_plot_summaries();
    bool is_plot_feedback_blocked() const;
    bool require_active_plot_target(SPDocument *&doc, SPDesktop *&desktop, bool clear_preview_on_failure = true);
    GrblPanelExportSession make_export_session(SPDesktop *desktop, std::atomic<bool> const *cancel = nullptr) const;
    bool prepare_active_export_target(SPDocument *&doc, SPDesktop *&desktop, GrblPanelExportSession &session,
                                      bool clear_preview_on_failure = true);
    void refresh_plot_feedback_after_gcode_change();
    Gtk::Window *get_dialog_parent_window(char const *missing_parent_message);
    GrblPanelSenderContext make_sender_context();
    void replace_editor_gcode_text(std::string const &text,
                                   std::optional<Inkscape::Axidraw::GrblExportParams> generated_params = std::nullopt);
    bool get_editor_gcode_text(std::string &text, bool send_from_cursor = false, guint *editor_line_1 = nullptr);
    void clear_editor_gcode_generation_state();
    void remember_editor_gcode_generation_state(Inkscape::Axidraw::GrblExportParams const &params);
    bool current_document_matches_editor_gcode_generation_state() const;
    bool current_mapping_matches_editor_gcode_generation_state() const;
    bool current_editor_gcode_generation_state_matches_active_context() const;
    void update_editor_gcode_generation_state_status();
    void handle_mapping_preferences_changed(bool refresh_preview);
    void handle_editor_gcode_changed(bool generated_from_document = false);
    void update_page_restore_button();
    void populate_bed_preset_combo();
    void apply_bed_preset_to_ui(std::string const &preset_id);
    void sync_bed_preset_from_dimensions();
    void update_bed_size_control_sensitivity(bool allow_interaction);
    bool get_configured_bed_size_mm(double &bed_width_mm, double &bed_height_mm) const;
    bool prepare_document_bed_action(SPDocument *&doc, Geom::Rect &bounds, double &bed_w_doc, double &bed_h_doc,
                                     Glib::ustring &error, Glib::ustring const &empty_message) const;
    void request_canvas_redraw() const;
    bool has_plot_preview_enabled() const;
    bool build_document_preview_overlay(SPDocument *doc, SPDesktop *desktop,
                                        Inkscape::Axidraw::GrblExportParams const &params,
                                        Inkscape::Axidraw::GrblExportContext const &ctx,
                                        Geom::Affine const &affine, Glib::ustring &status_note);
    bool build_machine_preview_overlay(SPDocument *doc, SPDesktop *desktop,
                                       Inkscape::Axidraw::GrblExportParams const &params,
                                       Inkscape::Axidraw::GrblExportContext const &ctx,
                                       Geom::Affine const &affine, Glib::ustring &status_note);
    bool build_machine_bed_overlay(SPDocument *doc, SPDesktop *desktop,
                                   Inkscape::Axidraw::GrblExportParams const &params,
                                   Geom::Affine const &affine);
    void build_machine_axis_overlay(SPDesktop *desktop, Inkscape::Axidraw::GrblExportParams const &params,
                                    Geom::Affine const &affine);
    void schedule_plot_feedback_refresh(bool refresh_preview = true);
    void schedule_plot_feedback_refresh(GrblPlotFeedbackChannels const &channels);
    void refresh_plot_feedback(bool refresh_preview = true);
    void refresh_plot_feedback(GrblPlotFeedbackChannels const &channels);
    void request_plot_feedback_for_trigger(GrblPlotFeedbackTrigger trigger, bool refresh_preview = true);
    bool begin_gcode_stream_ui(GrblGcodeStartOrigin origin);
    void post_gcode_stream_result(std::string const &err);
    void join_gcode_stream_thread();
    void start_gcode_stream_thread(std::function<void()> work);
    void run_gcode_stream_thread(std::function<void(std::unique_lock<std::mutex> &)> work);
    void finish_gcode_stream_worker(std::unique_lock<std::mutex> &port_lock);
    void with_grbl_plot_waits(std::function<void()> work);
    std::unique_ptr<GrblPanelWorkers> _workers;
    std::unique_ptr<Inkscape::Axidraw::GrblLink> _link;
    std::mutex _gcode_stream_thread_mutex;
    std::thread _gcode_stream_thread;
    std::mutex _port_mutex;
    std::atomic<bool> _connecting{false};
    std::atomic<bool> _gcode_sending{false};
    std::atomic<bool> _gcode_cancel{false};
    std::atomic<bool> _firmware_syncing{false};

    /// True while a background poll for '?'/status is running (at most one at a time).
    std::atomic<bool> _machine_status_poll_in_flight{false};

    sigc::connection _machine_status_poll;
    sigc::connection _delayed_firmware_sync;
    sigc::connection _plot_feedback_refresh_timer;
    sigc::connection _document_modified;
    bool _suspend_port_combo{false};
    bool _suspend_mapping_sync{false};
    bool _plot_feedback_refresh_summaries_requested{false};
    bool _plot_feedback_refresh_overlay_requested{false};
    bool _plot_feedback_refresh_dispatch_pending{false};
    bool _cancel_return_to_origin_pending{false};
    bool _suspend_editor_gcode_tracking{false};
    bool _editor_gcode_generation_state_warned_stale{false};
    GrblPageRestoreState _page_restore_state;
    SPDocument *_editor_gcode_generation_document = nullptr;
    EditorGcodeGenerationState _editor_gcode_generation_state;

    Gtk::Frame _frame;
    Gtk::Box _vbox{Gtk::Orientation::VERTICAL};
    Gtk::Box _port_row{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Label _port_lbl;
    Gtk::ComboBoxText _port_combo;
    Gtk::Button _btn_refresh_ports;
    Gtk::CheckButton _chk_canvas_plot_preview;
    Gtk::CheckButton _chk_machine_space_preview;
    Gtk::CheckButton _chk_send_from_cursor_line;
    Gtk::CheckButton _chk_sync_page_to_bed;
    Gtk::Label _machine_status;
    Gtk::ToggleButton _btn_connect;
    Gtk::Button _btn_read_firmware;
    Gtk::ComboBoxText _radio_mode_combo;
    Gtk::Entry _radio_pwd;
    Gtk::CheckButton _chk_radio_restart;
    Gtk::Button _btn_read_radio_mode;
    Gtk::Button _btn_read_ip;
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
    Gtk::Button _btn_fit_to_bed;
    Gtk::Button _btn_center_to_bed;
    Gtk::Button _btn_restore_page_size;
    Gtk::CheckButton _chk_swap_xy;
    Gtk::CheckButton _chk_invert_x;
    Gtk::CheckButton _chk_invert_y;
    Gtk::CheckButton _chk_flip_y;
    Gtk::ComboBoxText _plot_anchor_combo;
    Gtk::CheckButton _chk_clip_bed;
    Gtk::CheckButton _chk_lead_in;
    Gtk::CheckButton _chk_lead_out;
    Gtk::CheckButton _chk_contour_to_hatch;
    Gtk::CheckButton _chk_hatch_cross;
    Gtk::CheckButton _chk_hatch_inset;
    Gtk::CheckButton _chk_hatch_angle_increment;
    Gtk::CheckButton _chk_long_pen_up;
    Gtk::CheckButton _chk_near_connect;
    Gtk::CheckButton _chk_sparse_sampling;
    Gtk::ComboBoxText _tool_change_mode_combo;
    Gtk::CheckButton _chk_manual_pen_change_to_home;
    Gtk::CheckButton _chk_manual_pen_change_prompt;
    Gtk::CheckButton _chk_tool_change_point;
    Gtk::ComboBoxText _bed_preset_combo;
    Gtk::SpinButton _draw_feed_spin;
    Gtk::SpinButton _travel_feed_spin;
    Gtk::SpinButton _pen_up_delay_spin;
    Gtk::SpinButton _pen_down_delay_spin;
    Gtk::SpinButton _lead_in_dist_spin;
    Gtk::SpinButton _lead_out_dist_spin;
    Gtk::SpinButton _hatch_spacing_spin;
    Gtk::SpinButton _hatch_angle_spin;
    Gtk::SpinButton _hatch_inset_spin;
    Gtk::SpinButton _hatch_angle_increment_spin;
    Gtk::Entry _pen_up_cmd_entry;
    Gtk::Entry _pen_down_cmd_entry;
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
    Gtk::Label _job_summary;
    Gtk::Label _layout_scale_summary;
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
    CanvasItemPtr<Inkscape::CanvasItemBpath> _plot_preview_machine_bed_overlay;
    CanvasItemPtr<Inkscape::CanvasItemBpath> _plot_preview_machine_axis_overlay;
    CanvasItemPtr<Inkscape::CanvasItemText> _plot_preview_machine_bed_info_label;
    CanvasItemPtr<Inkscape::CanvasItemText> _plot_preview_machine_anchor_label;
    CanvasItemPtr<Inkscape::CanvasItemText> _plot_preview_axis_origin_label;
    CanvasItemPtr<Inkscape::CanvasItemText> _plot_preview_axis_x_label;
    CanvasItemPtr<Inkscape::CanvasItemText> _plot_preview_axis_y_label;
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_GRBL_CONTROL_PANEL_H
