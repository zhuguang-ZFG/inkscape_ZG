# GRBL Streaming Control Parity Design

Date: 2026-05-09

## Context

The first implementation slice will improve the Inkscape ZG GRBL control panel by learning the strongest streaming and control-console ideas from `D:\GIT\GRBL-Plotter`, without cloning unrelated subsystems such as probing, camera calibration, height maps, or advanced path generation.

Current Inkscape ZG already has a GRBL panel, serial transport, direct document send, editor send, cancel flow, runtime guards, firmware sync, preview overlays, job summaries, and GRBL export features such as hatch, lead-in/out, near-connect, sparse sampling, bed clipping, layer pause, and tool-change support.

The gap for this slice is streaming visibility and safety: the panel should make send progress, controller state, in-flight work, errors, alarms, pause, resume, and stop behavior explicit and testable.

## Goals

- Add a focused streaming state model for queued lines, acknowledged lines, in-flight lines, buffer occupancy, pause state, cancel state, and terminal error/alarm state.
- Parse GRBL replies through a narrow helper instead of scattering string guesses through UI code.
- Present streaming state as clear panel feedback: progress, buffer usage, controller phase, blocking reason, and safe button sensitivity.
- Preserve existing pen-up, cancel, work-origin, serial transport, and firmware sync behavior unless the new state model exposes an existing bug.
- Keep the implementation host-side unless later hardware testing shows a firmware-side mismatch.

## Non-Goals

- Do not clone GRBL-Plotter as a full application.
- Do not implement probing, height-map compensation, camera overlay, fiducial teaching, gamepad control, second GRBL hardware, or advanced tool/path-generation features in this slice.
- Do not rewrite the export pipeline, preview overlay system, or serial link abstraction unless required by the streaming state boundary.
- Do not claim hardware parity without real controller verification.

## Reference Features To Learn

From GRBL-Plotter, this slice should learn these concepts:

- Separate streaming state from UI widgets.
- Track file progress and controller buffer progress independently.
- Treat `ok`, `error:n`, `ALARM:n`, and runtime status reports as state transitions.
- Stop sending new lines immediately after terminal errors or alarms.
- Make pause, resume, and stop button availability derive from state, not from scattered booleans.
- Surface blocking reasons clearly instead of silently disabling actions.

Only those concepts are in scope. The implementation should adapt them to Inkscape ZG's C++/GTK structure and current `src/ui/dialog/grbl-*` helper pattern.

## Architecture

### Streaming State

Add a small host-side streaming state unit, likely under `src/ui/dialog/grbl-panel-streaming-*`, with a state object that tracks:

- total sendable lines
- lines written to the transport
- lines acknowledged by `ok`
- in-flight line count
- optional in-flight byte or buffer estimate if the existing sender exposes enough information safely
- pause requested
- cancel requested
- terminal error text
- terminal alarm text
- last runtime status text and parsed runtime phase

The state model should be deterministic and unit-testable without serial hardware.

### Reply Parsing

Add a parser helper that classifies incoming GRBL text into events:

- `ok`
- `error:n` with message text preserved
- `ALARM:n` with message text preserved
- runtime status report such as `<Idle|...>`, `<Run|...>`, `<Hold|...>`
- informational text such as firmware banners or settings replies
- unknown text

The parser should not decide UI behavior. It should only classify input.

### Presentation

Add a presentation helper that maps streaming state to UI decisions:

- progress label and fraction
- buffer/in-flight label and fraction when available
- status card text
- error/alarm card text
- send button sensitivity
- pause/resume/stop sensitivity
- tooltip or blocked reason

This keeps `grbl-control-panel.cpp` focused on GTK wiring.

### Panel Integration

Integrate the new helpers into the existing sender path:

- initialize streaming state before a send starts
- mark lines as written when the sender writes to the link
- advance acknowledgements when `ok` replies are observed
- enter blocked terminal state on `error` or `ALARM`
- pause by stopping new line feed while preserving current in-flight accounting
- resume only when connected, non-terminal, not cancelled, and paused
- stop by setting cancel state and reusing the existing safe cancel behavior for pen-up / return handling
- reset streaming state on disconnect or finished job

Existing runtime guards remain the source of truth for whether the panel is busy overall.

## UI Design Direction

The panel should gain a compact streaming status area instead of adding a large new window.

The status area should show:

- current job progress, for example `Sent 42 / 180 lines`
- in-flight or buffer status, for example `Awaiting 3 acknowledgements`
- controller phase, for example `Run`, `Hold`, `Idle`, or `Alarm`
- last blocking reason when blocked

Colors should be purposeful and restrained:

- normal/running: neutral or blue accent
- paused/hold: amber accent
- error/alarm: red accent
- complete/idle: green or neutral success accent

The visual style should follow the existing GTK/Inkscape panel conventions and current `grbl-panel-*` presentation helpers rather than introducing a separate design system.

## Error Handling

- `ok` without in-flight work should not crash; it should be ignored or surfaced as a diagnostic state depending on existing sender behavior.
- `error:n` should stop new line feed, preserve the original line context if available, and display the error.
- `ALARM:n` should stop new line feed and prevent resume until the user clears or reconnects according to existing GRBL recovery flow.
- Disconnect during streaming should clear in-flight state and return buttons to a safe disconnected state.
- Cancel should be idempotent; repeated stop requests must not enqueue duplicate safe-stop commands.

## Testing

Add or extend narrow tests under `testfiles/src/`:

- parser classifies `ok`, `error:n`, `ALARM:n`, runtime reports, and informational lines
- streaming state advances progress on `ok`
- `error` blocks further sending
- `ALARM` blocks resume
- pause prevents additional line feed but keeps in-flight state
- resume is allowed only from a valid paused non-terminal state
- cancel is idempotent
- presentation helper returns correct button states and labels for idle, running, paused, error, alarm, cancelled, and complete states

After implementation, run the narrow `grbl-*` tests first. If available in the local environment, run the project build command as a second verification pass.

## Hardware Verification

This slice can be mostly verified without hardware through parser, state, and presentation tests. Real controller verification remains required for:

- exact timing of pause/resume while GRBL has in-flight commands
- observed behavior after alarm recovery
- safe-stop behavior on the physical pen mechanism
- serial buffer behavior on the target firmware

Final reporting must separate tested host behavior from hardware-dependent behavior.
