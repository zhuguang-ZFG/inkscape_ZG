---
name: grbl-host-firmware-loop
description: Use when working across Inkscape ZG host code and the local Grbl_Esp32 firmware checkout for GRBL/AxiDraw behavior changes, serial or TCP protocol debugging, pen-control semantics, firmware sync, upload, regression verification, or Git handoff. Triggers include host-firmware联调, 上位机下位机联调, GRBL control panel issues, Z vs M3/M5 behavior, layer pause/tool change flow, and requests to build, flash, test, or prepare GitHub updates for either side.
---

# GRBL Host Firmware Loop

Use this skill to treat `D:\GIT\inkscape_ZG` and `D:\GIT\inkscape_ZG\Grbl_Esp32` as one debugging surface.

## Goals

- Reproduce behavior before changing code.
- Decide whether the issue belongs to host, firmware, or both.
- Verify with the narrowest useful command first.
- Keep host and firmware findings synchronized in the final report.

## Local Layout

Read [references/local-layout.md](references/local-layout.md) before doing substantial work. It records the local repo paths, common commands, and the boundary between host and firmware responsibilities.

## Scripts

Use the bundled PowerShell scripts for repeatable local actions:

- `scripts/build-host.ps1`
- `scripts/build-firmware.ps1`
- `scripts/upload-firmware.ps1`
- `scripts/serial-smoke.ps1`

Run them with `-DryRun` first when you need to confirm the exact command or target path.

## Default Loop

1. Read the user request and classify it as one of:
   - host-only
   - firmware-only
   - host-firmware interaction
2. Inspect the relevant host files first:
   - host pipeline: `src/axidraw/**`
   - host UI/control flow: `src/ui/dialog/grbl-*`
   - host notes: `src/axidraw/FIRMWARE.md`, `doc/grbl-*.md`, `BUILD-ENV-ZG.md`
3. If behavior depends on controller semantics, inspect the firmware side next:
   - `Grbl_Esp32/Grbl_Esp32/src`
   - `Grbl_Esp32/Grbl_Esp32/Custom`
   - `Grbl_Esp32/Grbl_Esp32/Machines`
4. State which side you believe owns the bug or optimization before editing.
5. Prefer the narrowest verification path:
   - host pure logic: existing `grbl-*` unit tests
   - host export semantics: CLI/headless export
   - firmware compile safety: `platformio run`
   - hardware behavior: upload only when explicitly requested or clearly required
6. After verification, report:
   - what changed on the host
   - what changed in firmware
   - what was verified
   - what still needs real hardware confirmation

## Decision Rules

- If the problem is about path ordering, clipping, hatch, preview, export text, or UI state, start from host code.
- If the problem is about `ok` timing, runtime replies, Z motor motion, `M3/M5` mapping, ESP commands, or machine-side persistence, inspect firmware semantics too.
- If the host UI is assuming a controller capability, confirm the matching firmware command or file before changing the assumption.
- Do not claim protocol parity with reverse-engineered Java hosts unless the code truly matches wire behavior.

## Verification Order

- Start with read-only inspection and an existing targeted test.
- Then run one build or one test command that proves the suspected area.
- Only after that, widen to integrated export checks or firmware build/upload.
- For hardware-sensitive changes, distinguish:
  - compiled successfully
  - exported/parsed correctly
  - observed on hardware

## Git Boundaries

- You may inspect both repos and prepare changes in either repo.
- Do not push, publish, or open GitHub updates unless the user explicitly asks for that action.
- When both repos change, summarize them separately so the user can decide whether to split commits or PRs.

## Common Tasks

- For host export or preview regressions, read:
  - `doc/grbl-cli-export.md`
  - `doc/grbl-preferences-reference.md`
  - `doc/grbl-control-panel-workflow.md`
- For pen semantics or ESP commands, read:
  - `src/axidraw/FIRMWARE.md`
  - `Grbl_Esp32/doc/Commands.txt`
- For local command recipes, read:
  - [references/local-layout.md](references/local-layout.md)
  - [references/verification-playbook.md](references/verification-playbook.md)
  - [references/script-recipes.md](references/script-recipes.md)

## Reporting Standard

Always separate conclusions into:

- host-side conclusion
- firmware-side conclusion
- verified evidence
- remaining uncertainty
