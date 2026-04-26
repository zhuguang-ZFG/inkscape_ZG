// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-runtime-state.h"

using Inkscape::UI::Dialog::GrblBusyReasonContext;
using Inkscape::UI::Dialog::GrblGcodeCancelPlan;
using Inkscape::UI::Dialog::GrblGcodeFinishPlan;
using Inkscape::UI::Dialog::GrblGcodeResultPlan;
using Inkscape::UI::Dialog::GrblGcodeStartOrigin;
using Inkscape::UI::Dialog::GrblGcodeStartPlan;
using Inkscape::UI::Dialog::GrblRuntimePhase;
using Inkscape::UI::Dialog::grbl_busy_reason_for_phase;
using Inkscape::UI::Dialog::make_grbl_gcode_cancel_plan;
using Inkscape::UI::Dialog::make_grbl_gcode_finish_plan;
using Inkscape::UI::Dialog::make_grbl_gcode_result_plan;
using Inkscape::UI::Dialog::make_grbl_gcode_start_plan;
using Inkscape::UI::Dialog::grbl_runtime_is_blocked;
using Inkscape::UI::Dialog::make_grbl_runtime_state_view;

TEST(GrblRuntimeStateTest, GcodeStateHasHighestPriority)
{
    auto const state = make_grbl_runtime_state_view(true, true, true, true);

    EXPECT_EQ(state.phase, GrblRuntimePhase::gcode_cancelling);
    EXPECT_TRUE(state.busy);
    EXPECT_TRUE(state.gcode_active);
    EXPECT_TRUE(state.cancel_requested);
}

TEST(GrblRuntimeStateTest, ConnectingBeatsFirmwareSyncWhenNoGcodeActive)
{
    auto const state = make_grbl_runtime_state_view(true, true, false, true);

    EXPECT_EQ(state.phase, GrblRuntimePhase::connecting);
    EXPECT_TRUE(state.busy);
    EXPECT_FALSE(state.gcode_active);
    EXPECT_FALSE(state.cancel_requested);
}

TEST(GrblRuntimeStateTest, BusyReasonVariesByContext)
{
    auto const generic = grbl_busy_reason_for_phase(GrblRuntimePhase::connecting, GrblBusyReasonContext::generic).raw();
    auto const send = grbl_busy_reason_for_phase(GrblRuntimePhase::connecting, GrblBusyReasonContext::send_action).raw();
    auto const export_action =
        grbl_busy_reason_for_phase(GrblRuntimePhase::firmware_sync, GrblBusyReasonContext::export_action).raw();

    EXPECT_NE(generic.find("稍候"), std::string::npos);
    EXPECT_NE(send.find("发送"), std::string::npos);
    EXPECT_NE(export_action.find("执行"), std::string::npos);
}

TEST(GrblRuntimeStateTest, BlockCheckReturnsReasonOnlyWhenRequestedDimensionsMatch)
{
    auto const state = make_grbl_runtime_state_view(false, true, false, false);
    Glib::ustring reason;

    EXPECT_TRUE(grbl_runtime_is_blocked(state, false, true, false, reason));
    EXPECT_FALSE(reason.empty());

    reason.clear();
    EXPECT_FALSE(grbl_runtime_is_blocked(state, true, false, true, reason));
    EXPECT_TRUE(reason.empty());
}

TEST(GrblRuntimeStateTest, CancelPlanOnlyPostsWhenNewCancelRequestStarts)
{
    auto const allowed = make_grbl_gcode_cancel_plan(true, false);
    EXPECT_TRUE(allowed.should_request);
    EXPECT_TRUE(allowed.post_status);
    EXPECT_FALSE(allowed.status.empty());

    auto const already_requested = make_grbl_gcode_cancel_plan(true, true);
    EXPECT_FALSE(already_requested.should_request);

    auto const inactive = make_grbl_gcode_cancel_plan(false, false);
    EXPECT_FALSE(inactive.should_request);
}

TEST(GrblRuntimeStateTest, StartPlanUsesOriginSpecificStatusWhenIdle)
{
    auto const state = make_grbl_runtime_state_view(false, false, false, false);

    auto const editor = make_grbl_gcode_start_plan(state, GrblGcodeStartOrigin::editor_gcode);
    EXPECT_TRUE(editor.allow_start);
    EXPECT_TRUE(editor.post_status);
    EXPECT_FALSE(editor.status_is_error);
    EXPECT_NE(editor.status.raw().find("编辑器"), std::string::npos);

    auto const direct = make_grbl_gcode_start_plan(state, GrblGcodeStartOrigin::document_direct);
    EXPECT_TRUE(direct.allow_start);
    EXPECT_NE(direct.status.raw().find("当前图稿"), std::string::npos);
}

TEST(GrblRuntimeStateTest, StartPlanBlocksWhenRuntimeBusy)
{
    auto const state = make_grbl_runtime_state_view(false, true, false, false);
    auto const plan = make_grbl_gcode_start_plan(state, GrblGcodeStartOrigin::editor_gcode);

    EXPECT_FALSE(plan.allow_start);
    EXPECT_TRUE(plan.post_status);
    EXPECT_TRUE(plan.status_is_error);
    EXPECT_FALSE(plan.status.empty());
}

TEST(GrblRuntimeStateTest, ResultPlanDistinguishesCancelAndFailure)
{
    auto const cancelled = make_grbl_gcode_result_plan(true, {});
    EXPECT_TRUE(cancelled.post_status);
    EXPECT_FALSE(cancelled.status_is_error);
    EXPECT_FALSE(cancelled.status.empty());

    auto const failed = make_grbl_gcode_result_plan(false, "boom");
    EXPECT_TRUE(failed.post_status);
    EXPECT_TRUE(failed.status_is_error);
    EXPECT_EQ(failed.status.raw(), "boom");
}

TEST(GrblRuntimeStateTest, FinishPlanTracksWorkerJoinRequirement)
{
    auto const worker = make_grbl_gcode_finish_plan(true);
    EXPECT_TRUE(worker.join_worker_thread);
    EXPECT_TRUE(worker.deactivate_stream);
    EXPECT_TRUE(worker.refresh_plot_feedback);

    auto const ui = make_grbl_gcode_finish_plan(false);
    EXPECT_FALSE(ui.join_worker_thread);
    EXPECT_TRUE(ui.deactivate_stream);
}
