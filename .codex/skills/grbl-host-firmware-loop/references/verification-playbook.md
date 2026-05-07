# Verification Playbook

## 1. Host-only logic change

Use the narrowest relevant unit test first.

Examples:

- `testfiles/src/grbl-export-test.cpp`
- `testfiles/src/grbl-runtime-state-test.cpp`
- `testfiles/src/grbl-panel-presentation-test.cpp`
- `testfiles/src/grbl-panel-transport-state-test.cpp`

If a specific test command is already known in this workspace, prefer it. Otherwise inspect `testfiles/CMakeLists.txt` and run the smallest matching target.

## 2. Export semantics change

Prefer a headless export check before hardware:

- confirm affected preferences or code path in `grbl_export_params_from_preferences(...)`
- generate or inspect G-code via CLI workflow documented in `doc/grbl-cli-export.md`
- if available, compare statistics in `GrblPlotStats`

Use staged debug only when needed:

- `INKSCAPE_GRBL_DEBUG_STAGES=1`
- `INKSCAPE_GRBL_DEBUG_DUMP_DIR=<dir>`

## 3. Control panel or sender flow change

Check both:

- state-transition tests for helper modules
- affected flow doc in `doc/grbl-control-panel-workflow.md`

If the bug concerns manual pen change, firmware sync, or connection status, verify the corresponding `grbl-panel-*` helper first before changing `grbl-control-panel.cpp`.

## 4. Firmware-only change

Run:

- `platformio run -e release`

If the change touches machine-specific behavior, identify the exact machine or spindle file involved and report that path explicitly.

## 5. Upload or hardware verification

Only upload when the user requests it or when the task clearly requires real-device validation.

Typical upload command:

- `platformio run -e release -t upload --upload-port COM3`

After upload, prefer a short explicit validation sequence, for example:

- `$$` to confirm persistent settings
- one or two small `G90/G1 Z...` checks for pen motion
- one small plotting sample before any long job

## 6. Git handoff

Before any GitHub push or PR preparation:

- inspect `git status --short` in each changed repo
- summarize host and firmware diffs separately
- call out anything unverified on hardware

Do not push by default. Push only on explicit user instruction.
