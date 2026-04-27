# AGENTS.md

Repository-wide instructions for Codex and compatible coding agents.

Unless the user explicitly asks otherwise, treat these rules as the default execution policy for planning, editing, reviewing, debugging, and refactoring work in this repository.

These instructions bias toward caution, narrow diffs, and verifiable outcomes over speed.

## 1. Think Before Coding

Do not silently guess.

- State assumptions explicitly before implementing when they matter.
- If the request is ambiguous, surface the main plausible interpretations.
- If a simpler approach exists, prefer it and say so.
- If missing information would materially change the implementation, stop and ask.

Prefer a short clarification or assumption list over confident guessing.

## 2. Keep It Simple

Implement the minimum code that solves the requested problem.

- Do not add features that were not requested.
- Do not introduce abstractions for single-use code.
- Do not add configurability or flexibility unless it is required.
- Do not add speculative error handling for unrealistic scenarios.

If the solution feels overengineered, simplify it.

## 3. Make Surgical Changes

Touch only what the request requires.

- Match the surrounding code style.
- Do not refactor adjacent code unless the request requires it.
- Do not rewrite comments, formatting, naming, or structure outside the necessary change set.
- Remove unused code only when your own changes made it unused.
- If you notice unrelated problems, mention them instead of fixing them opportunistically.

Every changed line should trace back to the user request or to verification for that request.

## 4. Drive Toward Verifiable Success

Turn vague tasks into concrete checks.

- For bugs, reproduce the issue first when practical.
- For behavior changes, identify the check that proves the new behavior.
- For refactors, verify behavior before and after.
- For multi-step work, use a short plan with a verification point for each step.

Prefer "write a failing test, fix it, make the test pass" over "make it work."

Use this compact template when helpful:

```text
1. [step] -> verify: [check]
2. [step] -> verify: [check]
3. [step] -> verify: [check]
```

## 5. Repository-Specific Notes

This repository is an Inkscape-based codebase with native GRBL/AxiDraw plotting additions. Many tasks affect both UI flow and machine-facing behavior.

- Prefer narrow, testable changes in `src/axidraw/**` and `src/ui/dialog/**`.
- Reuse existing helpers and state modules before adding new ones.
- When touching GRBL export, transport, pen control, firmware sync, or layered plotting flows, verify both logic correctness and user-facing workflow impact.
- Prefer adding or updating the narrowest relevant test in `testfiles/` when behavior changes.

## 6. GRBL Reference Paths

When modifying GRBL host-side behavior such as serial flow, `ok` protocol handling, pen control, tool change flow, layered pause logic, firmware sync, or related UI, also consult these local references when relevant:

- Firmware source: `D:\GIT\inkscape-axidraw\Grbl_Esp32`
- Reverse-engineered host reference: `D:\GIT\kxnx`
- Host/extension reference repository: `D:\GIT\inkscape-axidraw`
- Variant reference repository: `D:\GIT\inkscape_px`
- Repository note: `src/axidraw/FIRMWARE.md`

Do not repeatedly ask the user for these paths. If the paths change on the local machine, update `src/axidraw/FIRMWARE.md` and this file as needed.

## 7. Expected Reporting

When finishing non-trivial work, report:

- what changed
- what was verified
- any remaining uncertainty or unverified hardware-dependent behavior
