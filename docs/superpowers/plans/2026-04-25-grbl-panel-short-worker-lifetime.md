# GRBL Panel Short Worker Lifetime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent short-lived `GrblControlPanel` background workers from outliving the panel.

**Architecture:** Add a small `GrblPanelWorkers` lifetime boundary near the panel code. It owns short worker threads, exposes a stop flag, rejects new work during shutdown, and joins tracked short workers from the panel destructor. Long G-code streaming workers stay unchanged in this phase.

**Tech Stack:** C++20, `std::thread`, `std::atomic`, `std::mutex`, GoogleTest, CMake unit tests, existing GTK/Glib panel code.

**Git policy:** Do not create commits unless the user explicitly requests a commit.

---

## File Structure

- Create `src/ui/dialog/grbl-panel-workers.h`
  - Defines the small short-worker lifetime boundary used by `GrblControlPanel`.
- Create `src/ui/dialog/grbl-panel-workers.cpp`
  - Implements worker start, stop request, and join behavior.
- Modify `src/ui/CMakeLists.txt`
  - Adds the new worker source and header to `inkscape_base`.
- Create `testfiles/src/grbl-panel-workers-test.cpp`
  - Unit tests the worker boundary without constructing GTK dialogs.
- Modify `testfiles/CMakeLists.txt`
  - Adds a standalone unit test target for `grbl-panel-workers-test`.
- Modify `src/ui/dialog/grbl-control-panel.h`
  - Forward declares and owns `GrblPanelWorkers`.
- Modify `src/ui/dialog/grbl-control-panel.cpp`
  - Replaces short-task detached thread launches with `_workers->start(...)`.
  - Leaves `on_send_document_direct()` and `on_send_gcode()` detached streaming workers unchanged.

---

## Task 1: Add Failing Worker Boundary Tests

**Files:**
- Create: `testfiles/src/grbl-panel-workers-test.cpp`
- Modify: `testfiles/CMakeLists.txt`

- [ ] **Step 1: Write the failing test file**

Create `testfiles/src/grbl-panel-workers-test.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-workers.h"

#include <atomic>
#include <chrono>
#include <thread>

using Inkscape::UI::Dialog::GrblPanelWorkers;

TEST(GrblPanelWorkersTest, StartsWorkerAndJoinAllWaitsForCompletion)
{
    GrblPanelWorkers workers;
    std::atomic<bool> ran{false};

    ASSERT_TRUE(workers.start([&](std::atomic<bool> const &) {
        ran.store(true, std::memory_order_release);
    }));

    workers.join_all();

    EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(GrblPanelWorkersTest, RequestStopIsVisibleToWorker)
{
    GrblPanelWorkers workers;
    std::atomic<bool> saw_stop{false};

    ASSERT_TRUE(workers.start([&](std::atomic<bool> const &stop) {
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        saw_stop.store(true, std::memory_order_release);
    }));

    workers.request_stop();
    workers.join_all();

    EXPECT_TRUE(saw_stop.load(std::memory_order_acquire));
}

TEST(GrblPanelWorkersTest, RejectsNewWorkAfterStopRequest)
{
    GrblPanelWorkers workers;
    workers.request_stop();

    EXPECT_FALSE(workers.start([](std::atomic<bool> const &) {}));
}

TEST(GrblPanelWorkersTest, JoinAllWaitsForOutstandingWorker)
{
    GrblPanelWorkers workers;
    std::atomic<bool> finished{false};

    ASSERT_TRUE(workers.start([&](std::atomic<bool> const &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        finished.store(true, std::memory_order_release);
    }));

    workers.join_all();

    EXPECT_TRUE(finished.load(std::memory_order_acquire));
}
```

- [ ] **Step 2: Add the unit test target**

In `testfiles/CMakeLists.txt`, near the existing GRBL unit tests, add:

```cmake
add_unit_test(grbl-panel-workers-test TEST_SOURCE "grbl-panel-workers-test.cpp"
                                      SOURCES "ui/dialog/grbl-panel-workers.cpp")
```

Place it before `add_dependencies(tests unit_tests)`.

- [ ] **Step 3: Run the new test target and verify it fails for the expected reason**

Run:

```powershell
$env:MINGW_CHOST='x86_64-w64-mingw32'; $env:MINGW_PREFIX='C:/msys64/ucrt64'; $env:MSYSTEM='UCRT64'; $env:MINGW_PACKAGE_PREFIX='mingw-w64-ucrt-x86_64'; $env:Path='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + $env:Path; cmake --build build-zg --target grbl-panel-workers-test
```

Expected result: build fails because `src/ui/dialog/grbl-panel-workers.h` or `src/ui/dialog/grbl-panel-workers.cpp` does not exist yet.

---

## Task 2: Implement `GrblPanelWorkers`

**Files:**
- Create: `src/ui/dialog/grbl-panel-workers.h`
- Create: `src/ui/dialog/grbl-panel-workers.cpp`
- Modify: `src/ui/CMakeLists.txt`

- [ ] **Step 1: Write the worker boundary header**

Create `src/ui/dialog/grbl-panel-workers.h`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Short-lived worker lifetime boundary for GRBL panel tasks.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_WORKERS_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_WORKERS_H

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Inkscape::UI::Dialog {

class GrblPanelWorkers {
public:
    using StopFlag = std::atomic<bool>;
    using Work = std::function<void(StopFlag const &stop)>;

    GrblPanelWorkers() = default;
    ~GrblPanelWorkers();

    GrblPanelWorkers(GrblPanelWorkers const &) = delete;
    GrblPanelWorkers &operator=(GrblPanelWorkers const &) = delete;
    GrblPanelWorkers(GrblPanelWorkers &&) = delete;
    GrblPanelWorkers &operator=(GrblPanelWorkers &&) = delete;

    bool start(Work work);
    void request_stop() noexcept;
    void join_all() noexcept;
    [[nodiscard]] bool stop_requested() const noexcept;

private:
    mutable std::mutex _mutex;
    std::vector<std::thread> _threads;
    StopFlag _stop_requested{false};
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_WORKERS_H
```

- [ ] **Step 2: Write the worker boundary implementation**

Create `src/ui/dialog/grbl-panel-workers.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-workers.h"

#include <utility>

namespace Inkscape::UI::Dialog {

GrblPanelWorkers::~GrblPanelWorkers()
{
    request_stop();
    join_all();
}

bool GrblPanelWorkers::start(Work work)
{
    if (!work) {
        return false;
    }

    std::lock_guard const guard(_mutex);
    if (_stop_requested.load(std::memory_order_acquire)) {
        return false;
    }

    _threads.emplace_back([this, work = std::move(work)]() mutable {
        work(_stop_requested);
    });
    return true;
}

void GrblPanelWorkers::request_stop() noexcept
{
    _stop_requested.store(true, std::memory_order_release);
}

void GrblPanelWorkers::join_all() noexcept
{
    std::vector<std::thread> threads;
    {
        std::lock_guard const guard(_mutex);
        threads.swap(_threads);
    }

    for (auto &thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

bool GrblPanelWorkers::stop_requested() const noexcept
{
    return _stop_requested.load(std::memory_order_acquire);
}

} // namespace Inkscape::UI::Dialog
```

- [ ] **Step 3: Add the new files to `inkscape_base`**

In `src/ui/CMakeLists.txt`, add `dialog/grbl-panel-workers.cpp` next to `dialog/grbl-control-panel.cpp`:

```cmake
	dialog/debug.cpp
	dialog/grbl-control-panel.cpp
	dialog/grbl-panel-workers.cpp
	dialog/choose-file-utils.cpp
```

In the header section, add `dialog/grbl-panel-workers.h` next to `dialog/grbl-control-panel.h` if the file has a matching header list. If `dialog/grbl-control-panel.h` is not already listed, add both headers together:

```cmake
	dialog/grbl-control-panel.h
	dialog/grbl-panel-workers.h
```

- [ ] **Step 4: Run the worker unit test and verify it passes**

Run:

```powershell
$env:MINGW_CHOST='x86_64-w64-mingw32'; $env:MINGW_PREFIX='C:/msys64/ucrt64'; $env:MSYSTEM='UCRT64'; $env:MINGW_PACKAGE_PREFIX='mingw-w64-ucrt-x86_64'; $env:Path='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + $env:Path; cmake --build build-zg --target grbl-panel-workers-test; if ($LASTEXITCODE -eq 0) { .\build-zg\bin\grbl-panel-workers-test.exe }
```

Expected result: 4 tests pass.

---

## Task 3: Migrate Short Panel Workers

**Files:**
- Modify: `src/ui/dialog/grbl-control-panel.h`
- Modify: `src/ui/dialog/grbl-control-panel.cpp`

- [ ] **Step 1: Add worker ownership to the panel header**

In `src/ui/dialog/grbl-control-panel.h`, forward declare the worker:

```cpp
namespace Inkscape::UI::Dialog {

class GrblPanelWorkers;

class GrblControlPanel final : public DialogBase
{
```

Add the member near `_link`:

```cpp
    std::unique_ptr<Inkscape::UI::Dialog::GrblPanelWorkers> _workers;
    std::unique_ptr<Inkscape::Axidraw::GrblLink> _link;
```

- [ ] **Step 2: Include and initialize the worker boundary**

In `src/ui/dialog/grbl-control-panel.cpp`, add:

```cpp
#include "ui/dialog/grbl-panel-workers.h"
```

In `GrblControlPanel::GrblControlPanel()`, initialize `_workers` before `build_ui()`:

```cpp
{
    _workers = std::make_unique<GrblPanelWorkers>();
    _link = std::make_unique<Inkscape::Axidraw::GrblLink>();
    build_ui();
}
```

- [ ] **Step 3: Change destructor shutdown order**

Replace the destructor body with:

```cpp
GrblControlPanel::~GrblControlPanel()
{
    ensure_machine_status_poll(false);
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
```

This keeps `_link` close under the existing mutex and waits for short workers before GTK member destruction continues.

- [ ] **Step 4: Migrate machine status polling worker**

In `on_machine_status_poll_timeout()`, replace:

```cpp
    std::thread([this] {
        std::unique_lock<std::mutex> lk(_port_mutex, std::try_to_lock);
        if (!lk.owns_lock()) {
            return;
        }
        if (!(_link && _link->is_open())) {
            return;
        }
        char const q = '?';
        if (!_link->write_bytes(&q, 1)) {
            return;
        }
        std::string line;
        if (!_link->read_line(line, 400)) {
            return;
        }
        // If a prior command left an "ok" ahead of the report, read once more.
        if (line == "ok" && _link->read_line(line, 200)) {
            // use second line
        }
        post_machine_status(Glib::ustring(line));
    }).detach();
```

with:

```cpp
    if (!_workers || !_workers->start([this](GrblPanelWorkers::StopFlag const &stop) {
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
        return false;
    }
```

- [ ] **Step 5: Migrate `run_action()`**

Replace the detached thread in `run_action()` with:

```cpp
    if (!_workers || !_workers->start([this, w = std::move(work), report_ok](GrblPanelWorkers::StopFlag const &stop) mutable {
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            Glib::ustring blocked_reason;
            if (is_machine_command_blocked(blocked_reason)) {
                post_status(blocked_reason, true);
                return;
            }
            std::lock_guard const guard(_port_mutex);
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            if (is_machine_command_blocked(blocked_reason)) {
                post_status(blocked_reason, true);
                return;
            }
            if (!(_link && _link->is_open())) {
                post_not_connected_status();
                return;
            }
            std::string err;
            w(err);
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            if (err.empty()) {
                if (report_ok) {
                    post_status(_("操作完成。"), false);
                }
            } else {
                post_status(Glib::ustring(Inkscape::Axidraw::grbl_error_to_user_message(err)), true);
            }
        })) {
        post_status(_("当前面板正在关闭，无法启动新的控制器命令。"), true);
    }
```

- [ ] **Step 6: Migrate firmware sync worker**

In `on_read_firmware_settings()`, replace the outer `std::thread([this] { ... }).detach();` with `_workers->start`.

The start of the function should become:

```cpp
void GrblControlPanel::on_read_firmware_settings()
{
    if (!begin_firmware_sync()) {
        return;
    }
    if (!_workers || !_workers->start([this](GrblPanelWorkers::StopFlag const &stop) {
        scope_exit const finish_sync{[this] {
            Glib::signal_idle().connect_once(sigc::track_object([this] {
                set_firmware_syncing_state(false);
                if (_btn_connect.get_active()) {
                    ensure_machine_status_poll(true);
                }
                schedule_plot_feedback_refresh(false);
            }, *this));
        }};
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
```

Inside `query_lines_locked`, add stop checks before I/O and while reading:

```cpp
            if (stop.load(std::memory_order_acquire) || !(_link && _link->is_open())) {
                err_out = "not connected";
                return false;
            }

            _link->purge_io();
            if (stop.load(std::memory_order_acquire)) {
                err_out = "cancelled";
                return false;
            }

            bool const sent = _link && _link->write_line(command);
```

In the read loop, use:

```cpp
                if (stop.load(std::memory_order_acquire)) {
                    err_out = "cancelled";
                    return false;
                }
                std::string line;
                if (!_link->read_line(line, 1800)) {
                    err_out = "timeout waiting for controller response";
                    return false;
                }
```

Before posting the final snapshot UI callback, add:

```cpp
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
```

Close the worker launch with:

```cpp
    })) {
        set_firmware_syncing_state(false);
        post_status(_("当前面板正在关闭，无法同步固件参数。"), true);
    }
}
```

Keep the existing snapshot parsing and UI update logic otherwise unchanged.

- [ ] **Step 7: Migrate connection probe worker**

In `connect_toggle()`, replace the detached worker launch with `_workers->start`:

```cpp
    if (!_workers || !_workers->start([this, device_for_thread, baud, use_tcp, tcp_host, tcp_port](GrblPanelWorkers::StopFlag const &stop) {
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
        auto port = std::make_unique<SerialPort>();
        auto tcp = std::make_unique<Inkscape::Axidraw::TcpPort>();
        bool const opened = use_tcp ? tcp->open(tcp_host, tcp_port) : port->open(device_for_thread.raw(), baud);
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
        if (!opened) {
            Glib::signal_idle().connect_once(sigc::track_object([this, use_tcp] {
                finish_connect_attempt_ui(false, describe_connect_open_failure_ui(use_tcp), true);
            }, *this));
            return;
        }

        auto const probe = use_tcp ? Inkscape::Axidraw::probe_open_grbl(*tcp) : Inkscape::Axidraw::probe_open_grbl(*port);
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
        if (!probe.ok) {
            Glib::signal_idle().connect_once(sigc::track_object([this, probe, baud, dev = Glib::ustring(device_for_thread)] {
                auto const status = dev.rfind("tcp://", 0) == 0 ? describe_tcp_probe_failure_ui(dev, probe)
                                                                 : describe_probe_failure_ui(dev, baud, probe);
                finish_connect_attempt_ui(false, status, true, true);
            }, *this));
            return;
        }

        std::lock_guard guard(_port_mutex);
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
        if (use_tcp) {
            _link->set_tcp(std::move(tcp));
        } else {
            _link->set_serial(std::move(port));
        }
        Glib::signal_idle().connect_once(sigc::track_object([this, dev = Glib::ustring(device_for_thread), probe] {
            finalize_successful_connection_ui(dev, probe);
        }, *this));
    })) {
        set_connecting_state(false);
        _btn_connect.set_active(false);
        post_status(_("当前面板正在关闭，无法启动连接。"), true);
    }
```

- [ ] **Step 8: Confirm long streaming workers are unchanged**

Search:

```powershell
rg "std::thread|\\.detach\\(" src/ui/dialog/grbl-control-panel.cpp
```

Expected remaining detached workers are only the long streaming paths:

- `on_send_document_direct()`
- `on_send_gcode()`

If `on_machine_status_poll_timeout()`, `run_action()`, `on_read_firmware_settings()`, or `connect_toggle()` still show detached thread launches, finish the migration before continuing.

- [ ] **Step 9: Build the panel**

Run:

```powershell
$env:MINGW_CHOST='x86_64-w64-mingw32'; $env:MINGW_PREFIX='C:/msys64/ucrt64'; $env:MSYSTEM='UCRT64'; $env:MINGW_PACKAGE_PREFIX='mingw-w64-ucrt-x86_64'; $env:Path='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + $env:Path; (Get-Item "src\ui\dialog\grbl-control-panel.cpp").LastWriteTime = Get-Date; cmake --build build-zg --target inkscape
```

Expected result: `grbl-control-panel.cpp.obj`, `libinkscape_base.a`, and `inkscape.exe` build successfully.

---

## Task 4: Verification and Review

**Files:**
- Verify: `src/ui/dialog/grbl-panel-workers.h`
- Verify: `src/ui/dialog/grbl-panel-workers.cpp`
- Verify: `src/ui/dialog/grbl-control-panel.h`
- Verify: `src/ui/dialog/grbl-control-panel.cpp`
- Verify: `src/ui/CMakeLists.txt`
- Verify: `testfiles/CMakeLists.txt`
- Verify: `testfiles/src/grbl-panel-workers-test.cpp`

- [ ] **Step 1: Run the focused worker test**

Run:

```powershell
$env:Path='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + $env:Path; .\build-zg\bin\grbl-panel-workers-test.exe
```

Expected result: all `GrblPanelWorkersTest` tests pass.

- [ ] **Step 2: Run existing GRBL tests**

Run:

```powershell
$env:Path='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + $env:Path; .\build-zg\bin\grbl-client-test.exe; if ($LASTEXITCODE -eq 0) { .\build-zg\bin\test_grbl-export.exe }
```

Expected result:

- `grbl-client-test.exe`: all tests pass.
- `test_grbl-export.exe`: existing tests pass; `HeadlessRealWorldSvgSmoke` may remain skipped unless `INKSCAPE_GRBL_HEADLESS_SVG` is set.

- [ ] **Step 3: Check lints on touched files**

Use the IDE lint reader for:

- `src/ui/dialog/grbl-panel-workers.h`
- `src/ui/dialog/grbl-panel-workers.cpp`
- `src/ui/dialog/grbl-control-panel.h`
- `src/ui/dialog/grbl-control-panel.cpp`
- `src/ui/CMakeLists.txt`
- `testfiles/CMakeLists.txt`
- `testfiles/src/grbl-panel-workers-test.cpp`

Expected result: no new diagnostics in touched files.

- [ ] **Step 4: Check whitespace and diff scope**

Run:

```powershell
git diff --check -- src/ui/dialog/grbl-panel-workers.h src/ui/dialog/grbl-panel-workers.cpp src/ui/dialog/grbl-control-panel.h src/ui/dialog/grbl-control-panel.cpp src/ui/CMakeLists.txt testfiles/CMakeLists.txt testfiles/src/grbl-panel-workers-test.cpp
git diff --stat -- src/ui/dialog/grbl-panel-workers.h src/ui/dialog/grbl-panel-workers.cpp src/ui/dialog/grbl-control-panel.h src/ui/dialog/grbl-control-panel.cpp src/ui/CMakeLists.txt testfiles/CMakeLists.txt testfiles/src/grbl-panel-workers-test.cpp
```

Expected result:

- `git diff --check` exits with code 0.
- Diff scope is limited to the planned files.

- [ ] **Step 5: Request code review**

Dispatch a `code-reviewer` subagent with this scope:

- `src/ui/dialog/grbl-panel-workers.h`
- `src/ui/dialog/grbl-panel-workers.cpp`
- `src/ui/dialog/grbl-control-panel.h`
- `src/ui/dialog/grbl-control-panel.cpp`
- `src/ui/CMakeLists.txt`
- `testfiles/CMakeLists.txt`
- `testfiles/src/grbl-panel-workers-test.cpp`

Ask the reviewer to focus on:

- Whether short workers can still outlive `GrblControlPanel`.
- Whether shutdown can deadlock or post misleading UI status.
- Whether long streaming workers were accidentally changed.
- Whether tests cover the worker boundary behavior.

- [ ] **Step 6: Report residual manual checks**

Report these manual checks as follow-up because they need an actual controller or UI interaction:

- Start connection probe and close/disconnect quickly.
- Trigger firmware sync and close/disconnect quickly.
- Use jog/soft reset, then close the panel.
- Confirm G-code streaming behavior remains unchanged.

---

## Self-Review

- Spec coverage: The plan covers status polling, `run_action()`, firmware sync, connection probe, worker tests, build/test/lint verification, and explicitly excludes G-code streaming workers.
- Placeholder scan: No placeholder implementation steps are left.
- Type consistency: `GrblPanelWorkers`, `StopFlag`, `start`, `request_stop`, `join_all`, and `stop_requested` are named consistently across tasks.
- Scope check: This is one focused subsystem and can be implemented independently after the `GrblLink` extraction.
