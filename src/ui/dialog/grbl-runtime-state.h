// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared runtime state helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_RUNTIME_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_RUNTIME_STATE_H

#include <glibmm/ustring.h>

namespace Inkscape::UI::Dialog {

enum class GrblRuntimePhase {
    idle,
    connecting,
    firmware_sync,
    gcode_sending,
    gcode_cancelling,
};

struct GrblRuntimeStateView
{
    GrblRuntimePhase phase = GrblRuntimePhase::idle;
    bool busy = false;
    bool gcode_active = false;
    bool cancel_requested = false;
    bool connecting = false;
    bool firmware_sync = false;
};

struct GrblGcodeCancelPlan
{
    bool should_request = false;
    bool post_status = false;
    Glib::ustring status;
};

struct GrblGcodeResultPlan
{
    bool post_status = false;
    bool status_is_error = false;
    Glib::ustring status;
};

enum class GrblGcodeStartOrigin {
    editor_gcode,
    document_direct,
};

struct GrblGcodeStartPlan
{
    bool allow_start = false;
    bool post_status = false;
    bool status_is_error = false;
    Glib::ustring status;
};

struct GrblGcodeFinishPlan
{
    bool join_worker_thread = false;
    bool deactivate_stream = true;
    bool refresh_plot_feedback = true;
};

enum class GrblBusyReasonContext {
    generic,
    export_action,
    send_action,
};

GrblRuntimeStateView make_grbl_runtime_state_view(bool connecting, bool firmware_sync, bool gcode_active,
                                                  bool cancel_requested);
GrblGcodeStartPlan make_grbl_gcode_start_plan(GrblRuntimeStateView const &state, GrblGcodeStartOrigin origin);
GrblGcodeCancelPlan make_grbl_gcode_cancel_plan(bool gcode_active, bool cancel_already_requested);
GrblGcodeResultPlan make_grbl_gcode_result_plan(bool cancelled, Glib::ustring const &failure_status);
GrblGcodeFinishPlan make_grbl_gcode_finish_plan(bool from_worker);
Glib::ustring grbl_busy_reason_for_phase(GrblRuntimePhase phase, GrblBusyReasonContext context);
bool grbl_runtime_is_blocked(GrblRuntimeStateView const &state, bool block_connecting, bool block_firmware_sync,
                             bool block_gcode_sending, Glib::ustring &reason);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_RUNTIME_STATE_H
