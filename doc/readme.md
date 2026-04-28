# Developer documentation

Welcome to the documentation for (future and current) Inkscape developers.

For user-facing documentation, please refer to the [Inkscape website](https://inkscape.org/).

 <!-- the first two are stored outside of docs/ -->
- [Installing](../INSTALL.md)
- [Contributing and Developing](../CONTRIBUTING.md)
<!-- docs/ -->
- [Compiling Inkscape](./building/readme.md)
- [Style guide for developer documentation](./documentation_style.md)
- [Developing Inkscape with Visual Studio Code on Windows](./vscode/readme.md)
- Native GRBL / AxiDraw integration lives in `src/axidraw/`, with UI in `src/ui/dialog/grbl-control-panel.*`
  and related preferences under Input/Output.
- [GRBL / AxiDraw developer overview (ZH)](./grbl-developer-overview.md) — module map, entry points, and data flow.
- [GRBL preferences reference (ZH)](./grbl-preferences-reference.md) — `/options/grbl/*` to `GrblExportParams` mapping.
- [GRBL CLI export guide (ZH)](./grbl-cli-export.md) — `--export-grbl-gcode` usage and examples.
- [GRBL troubleshooting guide (ZH)](./grbl-troubleshooting.md) — common errors and diagnosis actions.
- [GRBL control panel workflow (ZH)](./grbl-control-panel-workflow.md) — connection/sync/send/cancel flow map.



Some short-lived or historical content can be found in the [Inkscape wiki](https://wiki.inkscape.org/).

TODO: We are currently working on moving content all relevant long-lived developer documentation from the Wiki to here in the Git repository.
