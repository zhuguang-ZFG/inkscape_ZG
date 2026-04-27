# ZG Windows Build Environment

## First-time setup

1. Install MSYS2 to `C:\msys64`.
2. Run:

```bat
setup-zg-build-env.cmd
```

This script will:

- install the required MSYS2 `UCRT64` packages from `msys2-ucrt64-packages.txt`
- configure `inkscape\build-zg`
- generate `build.ninja` and `compile_commands.json`

If you also want a full MSYS2 package upgrade before installing build dependencies:

```bat
setup-zg-build-env.cmd --upgrade
```

## Daily usage

Normal build:

```bat
build-zg-inkscape.cmd
```

Force CMake reconfigure:

```bat
build-zg-inkscape.cmd --reconfigure
```

Only regenerate build files without compiling:

```bat
build-zg-inkscape.cmd --reconfigure --configure-only
```

Open a ready-to-build terminal:

```bat
start-zg-dev-shell.cmd
```

## Notes

- `build-zg-inkscape.cmd` now auto-runs CMake if `build.ninja` is missing.
- The build is pinned to the MSYS2 `UCRT64` toolchain.
- `compile_commands.json` is enabled during configure for editor tooling.

## GRBL path optimization notes

- Current GRBL optimization work is in [inkscape/src/axidraw/pipeline/grbl-export.cpp](inkscape/src/axidraw/pipeline/grbl-export.cpp).
- Two recent effective improvements are now in the pipeline:
  - exact endpoint join for adjacent open strokes
  - endpoint reordering that considers GRBL output precision (`0.001 mm`)

### Why the quantized reorder matters

- GRBL coordinates are emitted with `%.3f`, so machine-visible endpoints are rounded to `0.001 mm`.
- For dense line art, nearest-neighbor decisions on full-precision geometry can differ from decisions on final emitted coordinates.
- The current optimization uses this quantized endpoint view to better match the actual travel path seen in exported G-code.

### Verified sample results

- `C:\Users\Administrator\Desktop\20230618 310mm x 410mm Prism x8  [Converted].svg`
  - stable stroke count: `8862`
  - draw length: about `348490.6 mm`
  - travel length improved from about `15099.8 mm` to about `14807.4 mm`
- `C:\Users\Administrator\Desktop\尾部.dxf`
  - stable stroke count: `3477`
  - draw length: about `142832.8 mm`
  - travel length improved from about `7692.0 mm` to about `7656.1 mm`
  - hatch diagnostics remain stable: `175 / 175`, `inset-applied: 175`, `inset-fallback: 0`

### Current local acceptance checks

- Acceptance runs below used the current `inkscape\build-zg\bin\inkscape.com`.
- To isolate stroke-order optimization itself, the test profiles used:
  - `clip-to-machine-bed=0`
  - `enable-near-connect=0`
  - `enable-sparse-stroke-sampling=0`
  - compare `optimize-stroke-order=1` vs `0`

#### `tmp\tail.dxf`

- optimize on:
  - strokes: `175`
  - draw/travel: `15391.3 / 2372.2 mm`
  - estimated duration: `780.7 s`
  - reported optimized travel: `17369.7 -> 2372.2 mm`
- optimize off:
  - strokes: `175`
  - draw/travel: `15391.3 / 17369.7 mm`
  - estimated duration: `930.6 s`

#### `tmp\tail_from_dxf.svg`

- optimize on:
  - strokes: `175`
  - draw/travel: `15391.3 / 2372.2 mm`
  - estimated duration: `780.7 s`
  - reported optimized travel: `17369.7 -> 2372.2 mm`
- optimize off:
  - strokes: `175`
  - draw/travel: `15391.3 / 17369.7 mm`
  - estimated duration: `930.6 s`

#### `tmp\dxf_script_stdout.svg`

- optimize on:
  - strokes: `175`
  - draw/travel: `15391.3 / 2372.2 mm`
  - estimated duration: `780.7 s`
  - reported optimized travel: `17369.7 -> 2372.2 mm`
- optimize off:
  - strokes: `175`
  - draw/travel: `15391.3 / 17369.7 mm`
  - estimated duration: `930.6 s`

#### `near-connect` spot check on `tmp\tail_from_dxf.svg`

- baseline (`near-connect=0`):
  - strokes: `175`
  - draw/travel: `15391.3 / 2372.2 mm`
  - estimated duration: `780.7 s`
- near-connect on (`near-connect-distance-mm=0.3`):
  - strokes: `175`
  - draw/travel: `15391.3 / 2260.1 mm`
  - estimated duration: `779.5 s`
- Interpretation:
  - `near-connect` gives a small extra travel reduction on this sample.
  - It is a geometry-changing strategy, not just pure reordering.

#### `sparse` spot check on `tmp\tail_from_dxf.svg`

- baseline (`sparse=0`):
  - strokes: `175`
  - draw/travel: `15391.3 / 2372.2 mm`
  - estimated duration: `780.7 s`
- sparse on (`sparse-keep-every=2`, `sparse-strategy=legacy`):
  - strokes: `88`
  - draw/travel: `8054.8 / 2017.2 mm`
  - estimated duration: `416.3 s`
- Interpretation:
  - `sparse` is effective for speed, but it reduces actual drawn strokes.
  - Treat it as a deliberate output-simplification mode, not as a pure path optimizer.

#### Layered sample spot check: `tmp\plot-3layer-T1.svg`

- optimize on:
  - strokes: `7`
  - draw/travel: `1044.8 / 195.3 mm`
  - estimated duration: `53.3 s`
  - reported optimized travel: `307.8 -> 195.3 mm`
- optimize off:
  - strokes: `7`
  - draw/travel: `1044.8 / 307.8 mm`
  - estimated duration: `54.5 s`
- Interpretation:
  - stroke-order optimization is still active and beneficial on this layered sample
  - stroke count remains stable

### Useful debug environment variables

- `INKSCAPE_GRBL_DEBUG_STAGES=1`
  - prints stage-by-stage stroke count, draw length, travel length, and bounds
- `INKSCAPE_GRBL_DEBUG_DUMP_DIR=D:\path\to\dump-dir`
  - when stage debug is enabled, writes TSV dumps of stroke order and endpoints for each stage

### Layered-mode testing note

- The current layered pause / per-layer tool-change path requires a live `desktop` context.
- Plain CLI export with `inkscape.com --export-grbl-gcode ...` does not enter that layered branch, even if the SVG has layer labels such as `T1`, `T2`, and the preferences enable layer tool changes.
- For layered behavior validation, prefer interactive desktop-driven testing instead of headless CLI-only checks.

Example:

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$env:INKSCAPE_PROFILE_DIR='D:\GIT\inkscape_ZG\tmp-profile-nohatch'
$env:INKSCAPE_GRBL_DEBUG_STAGES='1'
$env:INKSCAPE_GRBL_DEBUG_DUMP_DIR='D:\GIT\inkscape_ZG\tmp\grbl-dumps'
& 'D:\GIT\inkscape_ZG\inkscape\build-zg\bin\inkscape.com' `
  '--export-grbl-gcode' `
  '--export-filename=D:\GIT\inkscape_ZG\tmp\prism-current.gcode' `
  'C:\Users\Administrator\Desktop\20230618 310mm x 410mm Prism x8  [Converted].svg'
```

## Grbl_Esp32 integration notes

- Reference firmware source used for protocol checks: `D:\GIT\inkscape-axidraw\Grbl_Esp32`
- Serial command reference used during implementation: `D:\GIT\inkscape-axidraw\Grbl_Esp32\doc\Commands.txt`
- Current panel-side `Grbl_Esp32` support is in [src/ui/dialog/grbl-control-panel.cpp](src/ui/dialog/grbl-control-panel.cpp) and [src/ui/dialog/grbl-panel-firmware-sync.cpp](src/ui/dialog/grbl-panel-firmware-sync.cpp).

### Implemented panel helpers

- Wireless mode read/write uses `ESP110` for `STA / AP / BT / OFF`.
- Optional controller restart after radio changes uses `ESP444 RESTART`.
- "Read IP" uses `ESP111` and now:
  - shows the returned IP in the status line
  - writes `/options/grbl/net-host`
  - updates `/options/grbl/serial-device` to `tcp://<ip>:<port>`
  - refreshes the port combo and selects the TCP target
- "Sync firmware" now also queries:
  - `ESP800` for firmware / basic ESP info
  - `ESP420` for current ESP32 runtime / radio / service status

### Expected manual test flow

1. Connect to a `Grbl_Esp32` controller over serial or Bluetooth.
2. In the GRBL panel, keep the admin password field set to the controller password, usually `admin` unless changed.
3. Click "读取模式" and confirm the returned mode matches the controller state.
4. Click "读取 IP" and confirm:
   - the status line shows the returned IP
   - the port dropdown switches to a `tcp://...` entry
5. Click "同步绘图机参数" and confirm the firmware info area now includes `[ESP800]` and `[ESP420]` sections.
6. If desired, switch to `STA` or `AP`, optionally restart with `ESP444`, then reconnect using the auto-selected TCP target.

### Current limitations

- These additions were implemented from firmware source review and local code integration, but not yet verified against live hardware in this workspace.
- `ESP111`, `ESP800`, and `ESP420` are currently treated as plain text replies; no structured parser has been added yet.
- The panel does not yet expose `ESP410` AP scan, `ESP112` hostname editing, or HTTP/Telnet configuration commands.
