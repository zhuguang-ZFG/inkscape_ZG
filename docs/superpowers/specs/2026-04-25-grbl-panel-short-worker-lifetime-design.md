# GRBL Panel Short Worker Lifetime Design

## Goal

Reduce use-after-free risk in `GrblControlPanel` by bringing short-lived background workers under an explicit lifetime boundary.

This is a scoped follow-up to the `GrblLink` extraction. The new boundary should cover short controller tasks that currently use detached `std::thread` and capture `this`, while leaving long G-code streaming flows unchanged for a later phase.

## Scope

In scope:

- Machine status polling worker from `on_machine_status_poll_timeout()`.
- Short command worker launched by `run_action()`, including jog, soft reset, pen up/down, and similar controller commands.
- Firmware synchronization worker from `on_read_firmware_settings()`.
- Connection probe worker from `connect_toggle()`.
- Tests for the worker lifetime boundary itself.

Out of scope:

- `on_send_document_direct()` direct plot streaming.
- `on_send_gcode()` editor G-code streaming.
- Changes to GRBL command semantics, serial/TCP behavior, or UI copy.
- Large UI ownership refactors outside `GrblControlPanel`.

## Current Risk

The panel schedules several detached background threads that capture `this`. Many UI updates posted back to the main loop use `sigc::track_object`, which protects the queued UI callback, but the worker body itself can still access panel members after the dialog is destroyed.

The main risky access categories are:

- `_port_mutex` and `_link` access from background I/O workers.
- Runtime state checks such as `_connecting`, `_firmware_syncing`, and `_gcode_sending`.
- Calls that post status or schedule follow-up UI work from inside a worker.

The previous `GrblLink` extraction reduced transport coupling, but it did not change worker ownership.

## Proposed Design

Add a small worker lifetime boundary owned by `GrblControlPanel`, tentatively named `GrblPanelWorkers`.

Responsibilities:

- Start short-lived worker functions.
- Track active worker threads.
- Expose a shared stop flag that worker functions can check.
- Reject new work after shutdown starts.
- On shutdown, request stop and join all tracked short workers.

The panel destructor will follow this order:

1. Stop periodic UI timers.
2. Request worker shutdown.
3. Close `_link` under `_port_mutex` to unblock transport reads or writes where possible.
4. Join tracked short workers.
5. Let remaining GTK object destruction proceed.

This preserves the existing `_port_mutex` synchronization model. Workers still lock `_port_mutex` around link I/O, but they no longer outlive the panel.

## API Sketch

`GrblPanelWorkers` should live near the panel implementation unless tests require a small internal helper under `src/axidraw/device` or `src/ui/dialog`.

Expected shape:

```cpp
class GrblPanelWorkers {
public:
    GrblPanelWorkers();
    ~GrblPanelWorkers();

    GrblPanelWorkers(GrblPanelWorkers const &) = delete;
    GrblPanelWorkers &operator=(GrblPanelWorkers const &) = delete;

    bool start(std::function<void(std::atomic<bool> const &stop)> work);
    void request_stop();
    void join_all();
    bool stop_requested() const;

private:
    std::mutex _mutex;
    std::vector<std::thread> _threads;
    std::atomic<bool> _stop_requested{false};
};
```

The implementation may prune completed threads if needed, but the first version can keep the model simple: short workers are few, and `join_all()` at shutdown is enough.

## Panel Integration

`GrblControlPanel` will own a `std::unique_ptr<GrblPanelWorkers>` or direct member after GTK members as appropriate for destruction order.

Each migrated worker will use the boundary instead of `std::thread(...).detach()`:

- Status poll: if shutdown is requested, do nothing. Keep the current `try_to_lock` behavior.
- `run_action()`: check the stop flag before blocking on `_port_mutex` and after acquiring it.
- Firmware sync: use `scope_exit` to clear syncing state only if the panel is still alive through queued `sigc::track_object` callbacks; worker-level stop should avoid starting new I/O once shutdown begins.
- Connect probe: if stop is requested before posting UI completion, do not post success/failure callbacks.

Long streaming workers remain detached in this phase, but the design should not make them harder to migrate later.

## Error Handling

Shutdown is not an error state shown to the user. If the panel is closing, short workers should quietly stop or finish current I/O.

Transport failures keep existing user-facing messages. Closing `_link` during shutdown may cause a blocked read/write to fail; workers should observe the stop flag and avoid posting misleading connection errors during shutdown.

## Testing

Add unit tests for the worker boundary, not for GTK dialog lifetime:

- A started worker runs and can be joined.
- `request_stop()` becomes visible to a worker.
- Starting work after shutdown request fails.
- `join_all()` waits for outstanding workers.

Then verify integration through compilation and existing GRBL tests:

- Build `inkscape` so `grbl-control-panel.cpp` is compiled.
- Run `grbl-client-test.exe`.
- Run `test_grbl-export.exe`.
- Run lints on touched files.

Manual follow-up tests:

- Start connection probe and close/disconnect quickly.
- Trigger firmware sync and close/disconnect quickly.
- Use jog/soft reset, then close the panel.
- Confirm G-code streaming behavior remains unchanged.

## Non-Goals

This design does not attempt to make every panel operation cancellable immediately. Its purpose is to ensure short workers cannot outlive the panel. Long-running plot streams require a separate design because they use document pointers, progress callbacks, cancellation state, and `grbl_begin_plot_waits()` global hooks.

## Self-Review

- No placeholders or TBDs remain.
- Scope is limited to short worker lifetime management.
- The design preserves existing serial/TCP and GRBL behavior.
- The testing strategy avoids brittle headless GTK lifecycle tests while still proving the new boundary.
