# Local Layout

## Repo Paths

- Host repo: `D:\GIT\inkscape_ZG`
- Firmware repo used in this workspace: `D:\GIT\inkscape_ZG\Grbl_Esp32`
- Additional recorded references from host notes:
  - firmware reference clone: `D:\GIT\inkscape-axidraw\Grbl_Esp32`
  - reverse-engineered host reference: `D:\GIT\kxnx`
  - host/reference repo: `D:\GIT\inkscape-axidraw`
  - variant repo: `D:\GIT\inkscape_px`

## Host Areas

- export pipeline: `src/axidraw/pipeline/grbl-export.*`
- orchestration: `src/axidraw/core/*`
- transport: `src/axidraw/device/*`
- GRBL UI: `src/ui/dialog/grbl-control-panel.*`
- GRBL UI helper modules: `src/ui/dialog/grbl-panel-*`, `grbl-runtime-state.*`, `grbl-work-origin.*`, `grbl-pen-command.*`
- docs:
  - `doc/grbl-control-panel-workflow.md`
  - `doc/grbl-cli-export.md`
  - `doc/grbl-preferences-reference.md`
  - `BUILD-ENV-ZG.md`
  - `INSTALL-ZG.md`

## Firmware Areas

- root config and build: `platformio.ini`
- firmware tree: `Grbl_Esp32/`
- common files to inspect for behavior:
  - `Grbl_Esp32/src/Config.h`
  - `Grbl_Esp32/Machines/custom_3axis_hr4988.h`
  - `Grbl_Esp32/Custom/paixi_writer_tool_change.cpp`
  - `Grbl_Esp32/Spindles/NullSpindle.cpp`
- ESP command reference: `doc/Commands.txt`

## Known Tooling

- `platformio.exe` is available in `PATH`
- `git.exe` is available in `PATH`
- host build helpers in repo root:
  - `setup-zg-build-env.cmd`
  - `build-zg-inkscape.cmd`
  - `run-inkscape-newdll.cmd`
  - `validate-grbl-export.cmd`

## Command Patterns

### Host repo

- inspect status:
  - `git status --short`
- targeted file search:
  - `rg -n "GRBL|M3|M5|ESP111|tool change|layer pause" src doc`
- build host:
  - `build-zg-inkscape.cmd`

### Firmware repo

- inspect status:
  - `git status --short`
- build firmware:
  - `platformio run -e release`
- upload firmware:
  - `platformio run -e release -t upload --upload-port COM3`

## Responsibility Hints

- Host owns export semantics, preview semantics, UI state, and sender workflow.
- Firmware owns machine execution semantics, ESP commands, persistent settings, and the actual meaning of `M3/M5`, `Z`, and runtime replies.
- Problems around layer pause, manual pen change, or tool change often cross both sides.
