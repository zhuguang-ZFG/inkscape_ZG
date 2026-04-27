# GRBL Feedback Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split GRBL plot feedback refreshes into separate `summary` and `overlay` channels while keeping one semantic trigger entry point.

**Architecture:** Add a small pure planning layer for two-channel feedback refresh intent, then thread that intent through the existing debounce path without changing worker/threading architecture. Update `GrblControlPanel` to consume the new plan and keep trigger callsites semantic instead of open-coding summary/overlay behavior.

**Tech Stack:** C++, gtkmm, existing GRBL panel helper modules, gtest, CMake, Ninja via `build-zg-inkscape.cmd`

---

### Task 1: Add Two-Channel Feedback Plan Helper

**Files:**
- Create: `d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-panel-feedback-scope-state.h`
- Create: `d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-panel-feedback-scope-state.cpp`
- Modify: `d:\GIT\inkscape_ZG\inkscape\src\ui\CMakeLists.txt`
- Modify: `d:\GIT\inkscape_ZG\inkscape\testfiles\CMakeLists.txt`
- Create: `d:\GIT\inkscape_ZG\inkscape\testfiles\src\grbl-panel-feedback-scope-state-test.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// d:\GIT\inkscape_ZG\inkscape\testfiles\src\grbl-panel-feedback-scope-state-test.cpp
#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-feedback-scope-state.h"

using Inkscape::UI::Dialog::GrblPlotFeedbackChannels;
using Inkscape::UI::Dialog::make_grbl_plot_feedback_channels;

TEST(GrblPanelFeedbackScopeStateTest, FullTriggersRefreshSummaryAndOverlay)
{
    auto const channels = make_grbl_plot_feedback_channels(true, true);
    EXPECT_TRUE(channels.refresh_summaries);
    EXPECT_TRUE(channels.refresh_overlay);
}

TEST(GrblPanelFeedbackScopeStateTest, SummaryOnlyTriggerSkipsOverlay)
{
    auto const channels = make_grbl_plot_feedback_channels(true, false);
    EXPECT_TRUE(channels.refresh_summaries);
    EXPECT_FALSE(channels.refresh_overlay);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmd /c build-zg-inkscape.cmd grbl-panel-feedback-scope-state-test
```

Expected:

```text
FAIL or CMake error because grbl-panel-feedback-scope-state.cpp/.h do not exist yet
```

- [ ] **Step 3: Write minimal implementation**

```cpp
// d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-panel-feedback-scope-state.h
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_SCOPE_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_FEEDBACK_SCOPE_STATE_H

namespace Inkscape::UI::Dialog {

struct GrblPlotFeedbackChannels
{
    bool refresh_summaries = false;
    bool refresh_overlay = false;
};

GrblPlotFeedbackChannels make_grbl_plot_feedback_channels(bool refresh_summaries, bool refresh_overlay);

} // namespace Inkscape::UI::Dialog

#endif
```

```cpp
// d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-panel-feedback-scope-state.cpp
#include "grbl-panel-feedback-scope-state.h"

namespace Inkscape::UI::Dialog {

GrblPlotFeedbackChannels make_grbl_plot_feedback_channels(bool const refresh_summaries, bool const refresh_overlay)
{
    return {
        .refresh_summaries = refresh_summaries,
        .refresh_overlay = refresh_overlay,
    };
}

} // namespace Inkscape::UI::Dialog
```

```cmake
# d:\GIT\inkscape_ZG\inkscape\src\ui\CMakeLists.txt
dialog/grbl-panel-feedback-scope-state.cpp
dialog/grbl-panel-feedback-scope-state.h
```

```cmake
# d:\GIT\inkscape_ZG\inkscape\testfiles\CMakeLists.txt
add_unit_test(grbl-panel-feedback-scope-state-test TEST_SOURCE "grbl-panel-feedback-scope-state-test.cpp"
                                              SOURCES "ui/dialog/grbl-panel-feedback-scope-state.cpp")
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
cmd /c build-zg-inkscape.cmd grbl-panel-feedback-scope-state-test
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-scope-state-test.exe
```

Expected:

```text
Build succeeds and test exits with code 0
```

- [ ] **Step 5: Commit**

```bash
git -C d:\GIT\inkscape_ZG\inkscape add src/ui/CMakeLists.txt src/ui/dialog/grbl-panel-feedback-scope-state.cpp src/ui/dialog/grbl-panel-feedback-scope-state.h testfiles/CMakeLists.txt testfiles/src/grbl-panel-feedback-scope-state-test.cpp
git -C d:\GIT\inkscape_ZG\inkscape commit -m "Add GRBL feedback scope helper"
```

### Task 2: Extend Trigger Planning To Return Two-Channel Intent

**Files:**
- Modify: `d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-panel-feedback-trigger-state.h`
- Modify: `d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-panel-feedback-trigger-state.cpp`
- Modify: `d:\GIT\inkscape_ZG\inkscape\testfiles\src\grbl-panel-feedback-trigger-state-test.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST(GrblPanelFeedbackTriggerStateTest, MappingTriggerCanRequestSummaryOnly)
{
    auto const plan = make_grbl_plot_feedback_trigger_plan(
        GrblPlotFeedbackTrigger::mapping_preferences_changed, false);
    EXPECT_TRUE(plan.channels.refresh_summaries);
    EXPECT_FALSE(plan.channels.refresh_overlay);
    EXPECT_TRUE(plan.refresh_immediately);
}

TEST(GrblPanelFeedbackTriggerStateTest, LayoutTriggerRefreshesBothChannels)
{
    auto const plan = make_grbl_plot_feedback_trigger_plan(
        GrblPlotFeedbackTrigger::layout_changed, true);
    EXPECT_TRUE(plan.channels.refresh_summaries);
    EXPECT_TRUE(plan.channels.refresh_overlay);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmd /c build-zg-inkscape.cmd grbl-panel-feedback-trigger-state-test
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-trigger-state-test.exe
```

Expected:

```text
FAIL because trigger plan does not expose per-channel refresh state yet
```

- [ ] **Step 3: Write minimal implementation**

```cpp
// in grbl-panel-feedback-trigger-state.h
#include "ui/dialog/grbl-panel-feedback-scope-state.h"

struct GrblPlotFeedbackTriggerPlan
{
    GrblPlotFeedbackChannels channels;
    bool refresh_immediately = false;
};
```

```cpp
// in grbl-panel-feedback-trigger-state.cpp
GrblPlotFeedbackTriggerPlan make_grbl_plot_feedback_trigger_plan(GrblPlotFeedbackTrigger const trigger,
                                                                 bool const refresh_preview)
{
    GrblPlotFeedbackTriggerPlan plan;
    switch (trigger) {
        case GrblPlotFeedbackTrigger::document_content_changed:
        case GrblPlotFeedbackTrigger::selection_changed:
        case GrblPlotFeedbackTrigger::preview_visibility_changed:
        case GrblPlotFeedbackTrigger::layout_changed:
        case GrblPlotFeedbackTrigger::work_origin_changed:
            plan.channels = make_grbl_plot_feedback_channels(true, true);
            break;
        case GrblPlotFeedbackTrigger::mapping_preferences_changed:
            plan.channels = make_grbl_plot_feedback_channels(true, refresh_preview);
            plan.refresh_immediately = true;
            break;
    }
    return plan;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
cmd /c build-zg-inkscape.cmd grbl-panel-feedback-trigger-state-test
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-trigger-state-test.exe
```

Expected:

```text
Build succeeds and test exits with code 0
```

- [ ] **Step 5: Commit**

```bash
git -C d:\GIT\inkscape_ZG\inkscape add src/ui/dialog/grbl-panel-feedback-trigger-state.cpp src/ui/dialog/grbl-panel-feedback-trigger-state.h testfiles/src/grbl-panel-feedback-trigger-state-test.cpp
git -C d:\GIT\inkscape_ZG\inkscape commit -m "Split GRBL trigger refresh channels"
```

### Task 3: Carry Two-Channel Intent Through Debounce State

**Files:**
- Modify: `d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-panel-feedback-state.h`
- Modify: `d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-panel-feedback-state.cpp`
- Modify: `d:\GIT\inkscape_ZG\inkscape\testfiles\src\grbl-panel-feedback-state-test.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST(GrblPanelFeedbackStateTest, SchedulePlanRetainsPendingOverlayAndSummarySeparately)
{
    auto const plan = make_grbl_plot_feedback_schedule_plan(false, false, true, false, false, false, true);
    EXPECT_TRUE(plan.refresh_summaries_requested);
    EXPECT_TRUE(plan.refresh_overlay_requested);
}

TEST(GrblPanelFeedbackStateTest, TimerPlanCanRefreshSummaryWithoutOverlay)
{
    auto const plan = make_grbl_plot_feedback_timer_plan(true, false, true);
    EXPECT_TRUE(plan.refresh_summaries);
    EXPECT_FALSE(plan.sync_preview_overlay);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmd /c build-zg-inkscape.cmd grbl-panel-feedback-state-test
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-state-test.exe
```

Expected:

```text
FAIL because feedback state still uses a single preview latch
```

- [ ] **Step 3: Write minimal implementation**

```cpp
// reshape feedback state structs to carry two booleans
struct GrblPlotFeedbackSchedulePlan
{
    bool refresh_summaries_requested = false;
    bool refresh_overlay_requested = false;
    bool clear_preview_overlay = false;
    bool queue_idle_refresh = false;
};
```

```cpp
GrblPlotFeedbackSchedulePlan make_grbl_plot_feedback_schedule_plan(bool summaries_already_requested,
                                                                  bool overlay_already_requested,
                                                                  bool blocked,
                                                                  bool refresh_summaries,
                                                                  bool refresh_overlay,
                                                                  bool dispatch_pending,
                                                                  bool timer_connected);
```

```cpp
GrblPlotFeedbackTimerPlan make_grbl_plot_feedback_timer_plan(bool refresh_summaries_requested,
                                                             bool refresh_overlay_requested,
                                                             bool preview_enabled);
```

```cpp
// implementation rule:
// - summaries and overlay latches coalesce independently
// - clear_preview_overlay only when overlay refresh is requested
// - timer refreshes summaries whenever requested
// - timer syncs overlay only when overlay requested and preview enabled
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
cmd /c build-zg-inkscape.cmd grbl-panel-feedback-state-test
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-state-test.exe
```

Expected:

```text
Build succeeds and test exits with code 0
```

- [ ] **Step 5: Commit**

```bash
git -C d:\GIT\inkscape_ZG\inkscape add src/ui/dialog/grbl-panel-feedback-state.cpp src/ui/dialog/grbl-panel-feedback-state.h testfiles/src/grbl-panel-feedback-state-test.cpp
git -C d:\GIT\inkscape_ZG\inkscape commit -m "Track GRBL feedback channels separately"
```

### Task 4: Update GrblControlPanel To Use Separate Summary And Overlay Requests

**Files:**
- Modify: `d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-control-panel.h`
- Modify: `d:\GIT\inkscape_ZG\inkscape\src\ui\dialog\grbl-control-panel.cpp`

- [ ] **Step 1: Write the failing test**

There is no direct `GrblControlPanel` unit harness here, so use focused regression through existing plan-layer tests first, then rely on compile-time integration and related unit tests.

Add this note as a code comment only if needed during implementation:

```cpp
// The control panel now carries summary and overlay requests independently.
```

- [ ] **Step 2: Run integration-adjacent tests before implementation**

Run:

```bash
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-state-test.exe
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-trigger-state-test.exe
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-mapping-state-test.exe
```

Expected:

```text
All pass before control-panel integration changes
```

- [ ] **Step 3: Write minimal implementation**

```cpp
// grbl-control-panel.h
bool _plot_feedback_refresh_summaries_requested{false};
bool _plot_feedback_refresh_overlay_requested{false};
```

```cpp
// grbl-control-panel.cpp
void GrblControlPanel::request_plot_feedback_for_trigger(GrblPlotFeedbackTrigger const trigger,
                                                         bool const refresh_preview)
{
    auto const plan = make_grbl_plot_feedback_trigger_plan(trigger, refresh_preview);
    if (plan.refresh_immediately) {
        refresh_plot_feedback(plan.channels.refresh_overlay);
        return;
    }
    schedule_plot_feedback_refresh(plan.channels.refresh_overlay);
}
```

```cpp
// adapt schedule_plot_feedback_refresh / refresh_plot_feedback internals:
// - feed summary/overlay booleans into plan helpers
// - refresh_plot_summaries() only when requested
// - sync_plot_preview_overlay() only when requested
// - clear preview overlay only for overlay refresh requests
```

```cpp
// preserve public signatures if possible in this round,
// but internally derive:
// summaries = true for all current callsites
// overlay = refresh_preview argument
```

- [ ] **Step 4: Run focused verification**

Run:

```bash
cmd /c build-zg-inkscape.cmd grbl-panel-feedback-trigger-state-test
cmd /c build-zg-inkscape.cmd grbl-panel-feedback-state-test
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-trigger-state-test.exe
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-state-test.exe
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-mapping-state-test.exe
```

Expected:

```text
All pass with the control panel compiling against the new channel model
```

- [ ] **Step 5: Commit**

```bash
git -C d:\GIT\inkscape_ZG\inkscape add src/ui/dialog/grbl-control-panel.cpp src/ui/dialog/grbl-control-panel.h
git -C d:\GIT\inkscape_ZG\inkscape commit -m "Use split GRBL feedback refresh channels"
```

### Task 5: Full Build, Outer Repo Sync, And Final Verification

**Files:**
- Modify: `d:\GIT\inkscape_ZG\src\ui\CMakeLists.txt`
- Modify: `d:\GIT\inkscape_ZG\src\ui\dialog\grbl-control-panel.cpp`
- Modify: `d:\GIT\inkscape_ZG\src\ui\dialog\grbl-control-panel.h`
- Modify: `d:\GIT\inkscape_ZG\src\ui\dialog\grbl-panel-feedback-state.h`
- Modify: `d:\GIT\inkscape_ZG\src\ui\dialog\grbl-panel-feedback-state.cpp`
- Modify: `d:\GIT\inkscape_ZG\src\ui\dialog\grbl-panel-feedback-trigger-state.h`
- Modify: `d:\GIT\inkscape_ZG\src\ui\dialog\grbl-panel-feedback-trigger-state.cpp`
- Create: `d:\GIT\inkscape_ZG\src\ui\dialog\grbl-panel-feedback-scope-state.h`
- Create: `d:\GIT\inkscape_ZG\src\ui\dialog\grbl-panel-feedback-scope-state.cpp`
- Modify: `d:\GIT\inkscape_ZG\testfiles\CMakeLists.txt`
- Modify: `d:\GIT\inkscape_ZG\testfiles\src\grbl-panel-feedback-state-test.cpp`
- Modify: `d:\GIT\inkscape_ZG\testfiles\src\grbl-panel-feedback-trigger-state-test.cpp`
- Create: `d:\GIT\inkscape_ZG\testfiles\src\grbl-panel-feedback-scope-state-test.cpp`

- [ ] **Step 1: Copy verified inner-repo files to outer mirror**

Run:

```powershell
$src='d:\GIT\inkscape_ZG\inkscape'
$dst='d:\GIT\inkscape_ZG'
Copy-Item "$src\src\ui\CMakeLists.txt" "$dst\src\ui\CMakeLists.txt" -Force
Copy-Item "$src\src\ui\dialog\grbl-control-panel.cpp" "$dst\src\ui\dialog\grbl-control-panel.cpp" -Force
Copy-Item "$src\src\ui\dialog\grbl-control-panel.h" "$dst\src\ui\dialog\grbl-control-panel.h" -Force
Copy-Item "$src\src\ui\dialog\grbl-panel-feedback-state.cpp" "$dst\src\ui\dialog\grbl-panel-feedback-state.cpp" -Force
Copy-Item "$src\src\ui\dialog\grbl-panel-feedback-state.h" "$dst\src\ui\dialog\grbl-panel-feedback-state.h" -Force
Copy-Item "$src\src\ui\dialog\grbl-panel-feedback-trigger-state.cpp" "$dst\src\ui\dialog\grbl-panel-feedback-trigger-state.cpp" -Force
Copy-Item "$src\src\ui\dialog\grbl-panel-feedback-trigger-state.h" "$dst\src\ui\dialog\grbl-panel-feedback-trigger-state.h" -Force
Copy-Item "$src\src\ui\dialog\grbl-panel-feedback-scope-state.cpp" "$dst\src\ui\dialog\grbl-panel-feedback-scope-state.cpp" -Force
Copy-Item "$src\src\ui\dialog\grbl-panel-feedback-scope-state.h" "$dst\src\ui\dialog\grbl-panel-feedback-scope-state.h" -Force
Copy-Item "$src\testfiles\CMakeLists.txt" "$dst\testfiles\CMakeLists.txt" -Force
Copy-Item "$src\testfiles\src\grbl-panel-feedback-state-test.cpp" "$dst\testfiles\src\grbl-panel-feedback-state-test.cpp" -Force
Copy-Item "$src\testfiles\src\grbl-panel-feedback-trigger-state-test.cpp" "$dst\testfiles\src\grbl-panel-feedback-trigger-state-test.cpp" -Force
Copy-Item "$src\testfiles\src\grbl-panel-feedback-scope-state-test.cpp" "$dst\testfiles\src\grbl-panel-feedback-scope-state-test.cpp" -Force
```

Expected:

```text
Outer mirror files match the verified inner repo state
```

- [ ] **Step 2: Run full verification**

Run:

```bash
cmd /c build-zg-inkscape.cmd
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-scope-state-test.exe
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-trigger-state-test.exe
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-feedback-state-test.exe
d:\GIT\inkscape_ZG\inkscape\build-zg\bin\grbl-panel-mapping-state-test.exe
```

Expected:

```text
Full build succeeds and all targeted tests exit with code 0
```

- [ ] **Step 3: Check repo status**

Run:

```bash
git -C d:\GIT\inkscape_ZG\inkscape status --short
git -C d:\GIT\inkscape_ZG status --short
```

Expected:

```text
Inner repo shows only this round's intended changes
Outer repo shows only this round's intended changes, plus any pre-existing ignored/untracked nested repo entries
```

- [ ] **Step 4: Commit**

```bash
git -C d:\GIT\inkscape_ZG\inkscape add src/ui/CMakeLists.txt src/ui/dialog/grbl-control-panel.cpp src/ui/dialog/grbl-control-panel.h src/ui/dialog/grbl-panel-feedback-state.cpp src/ui/dialog/grbl-panel-feedback-state.h src/ui/dialog/grbl-panel-feedback-trigger-state.cpp src/ui/dialog/grbl-panel-feedback-trigger-state.h src/ui/dialog/grbl-panel-feedback-scope-state.cpp src/ui/dialog/grbl-panel-feedback-scope-state.h testfiles/CMakeLists.txt testfiles/src/grbl-panel-feedback-state-test.cpp testfiles/src/grbl-panel-feedback-trigger-state-test.cpp testfiles/src/grbl-panel-feedback-scope-state-test.cpp
git -C d:\GIT\inkscape_ZG\inkscape commit -m "Split GRBL summary and overlay refreshes"

git -C d:\GIT\inkscape_ZG add src/ui/CMakeLists.txt src/ui/dialog/grbl-control-panel.cpp src/ui/dialog/grbl-control-panel.h src/ui/dialog/grbl-panel-feedback-state.cpp src/ui/dialog/grbl-panel-feedback-state.h src/ui/dialog/grbl-panel-feedback-trigger-state.cpp src/ui/dialog/grbl-panel-feedback-trigger-state.h src/ui/dialog/grbl-panel-feedback-scope-state.cpp src/ui/dialog/grbl-panel-feedback-scope-state.h testfiles/CMakeLists.txt testfiles/src/grbl-panel-feedback-state-test.cpp testfiles/src/grbl-panel-feedback-trigger-state-test.cpp testfiles/src/grbl-panel-feedback-scope-state-test.cpp
git -C d:\GIT\inkscape_ZG commit -m "Split GRBL summary and overlay refreshes"
```

- [ ] **Step 5: Push**

```bash
git -C d:\GIT\inkscape_ZG\inkscape push github HEAD:refs/heads/inkscape-zg
git -C d:\GIT\inkscape_ZG push origin main
```

Expected:

```text
Both pushes succeed and remote heads advance to the new commits
```
