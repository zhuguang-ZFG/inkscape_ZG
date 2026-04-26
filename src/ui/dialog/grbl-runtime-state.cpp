// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-runtime-state.h"

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {

GrblRuntimeStateView make_grbl_runtime_state_view(bool const connecting, bool const firmware_sync,
                                                  bool const gcode_active, bool const cancel_requested)
{
    GrblRuntimeStateView state;
    state.connecting = connecting;
    state.firmware_sync = firmware_sync;
    state.gcode_active = gcode_active;
    state.cancel_requested = gcode_active && cancel_requested;

    if (state.gcode_active) {
        state.phase = state.cancel_requested ? GrblRuntimePhase::gcode_cancelling : GrblRuntimePhase::gcode_sending;
    } else if (state.connecting) {
        state.phase = GrblRuntimePhase::connecting;
    } else if (state.firmware_sync) {
        state.phase = GrblRuntimePhase::firmware_sync;
    } else {
        state.phase = GrblRuntimePhase::idle;
    }

    state.busy = state.phase != GrblRuntimePhase::idle;
    return state;
}

GrblGcodeStartPlan make_grbl_gcode_start_plan(GrblRuntimeStateView const &state, GrblGcodeStartOrigin const origin)
{
    GrblGcodeStartPlan plan;
    plan.post_status = true;
    if (state.busy) {
        plan.status_is_error = true;
        plan.status = grbl_busy_reason_for_phase(state.phase, GrblBusyReasonContext::send_action);
        return plan;
    }

    plan.allow_start = true;
    switch (origin) {
        case GrblGcodeStartOrigin::document_direct:
            plan.status = _("正在按当前图稿直接流式发送到绘图机...");
            break;
        case GrblGcodeStartOrigin::editor_gcode:
        default:
            plan.status = _("正在发送编辑器中的 G-code...");
            break;
    }
    return plan;
}

GrblGcodeCancelPlan make_grbl_gcode_cancel_plan(bool const gcode_active, bool const cancel_already_requested)
{
    GrblGcodeCancelPlan plan;
    if (!gcode_active || cancel_already_requested) {
        return plan;
    }

    plan.should_request = true;
    plan.post_status = true;
    plan.status = _("正在请求停止发送...");
    return plan;
}

GrblGcodeResultPlan make_grbl_gcode_result_plan(bool const cancelled, Glib::ustring const &failure_status)
{
    GrblGcodeResultPlan plan;
    plan.post_status = true;
    if (cancelled) {
        plan.status = _("发送已停止（已取消）。");
        return plan;
    }

    plan.status_is_error = true;
    plan.status = failure_status;
    return plan;
}

GrblGcodeFinishPlan make_grbl_gcode_finish_plan(bool const from_worker)
{
    GrblGcodeFinishPlan plan;
    plan.join_worker_thread = from_worker;
    return plan;
}

Glib::ustring grbl_busy_reason_for_phase(GrblRuntimePhase const phase, GrblBusyReasonContext const context)
{
    switch (phase) {
        case GrblRuntimePhase::connecting:
            if (context == GrblBusyReasonContext::send_action) {
                return _("当前正在连接绘图机，请等连接完成后再发送。");
            }
            if (context == GrblBusyReasonContext::export_action) {
                return _("当前正在连接绘图机，请等连接完成后再执行。");
            }
            return _("当前正在连接绘图机，请稍候再试。");
        case GrblRuntimePhase::firmware_sync:
            if (context == GrblBusyReasonContext::send_action) {
                return _("当前正在同步固件参数，请等同步完成后再发送。");
            }
            if (context == GrblBusyReasonContext::export_action) {
                return _("当前正在同步固件参数，请等同步完成后再执行。");
            }
            return _("当前正在同步固件参数，请稍候再试。");
        case GrblRuntimePhase::gcode_sending:
        case GrblRuntimePhase::gcode_cancelling:
            return _("当前正在发送任务，请先等待完成或取消发送。");
        case GrblRuntimePhase::idle:
        default:
            return {};
    }
}

bool grbl_runtime_is_blocked(GrblRuntimeStateView const &state, bool const block_connecting,
                             bool const block_firmware_sync, bool const block_gcode_sending,
                             Glib::ustring &reason)
{
    bool const blocked = (block_connecting && state.connecting) ||
                         (block_firmware_sync && state.firmware_sync) ||
                         (block_gcode_sending && state.gcode_active);
    if (!blocked) {
        return false;
    }

    reason = grbl_busy_reason_for_phase(state.phase, GrblBusyReasonContext::generic);
    return true;
}

} // namespace Inkscape::UI::Dialog
