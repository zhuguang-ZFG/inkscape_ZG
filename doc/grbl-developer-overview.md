# GRBL / AxiDraw 开发总览

面向在本仓库中修改 **原生 GRBL / AxiDraw** 功能的开发者：说明模块位置、主要入口、数据流与延伸阅读。参数键完整对照见 `doc/grbl-preferences-reference.md`。

## 1. 代码放在哪里

| 区域 | 路径 | 职责摘要 |
|------|------|----------|
| 编排（菜单直发） | `src/axidraw/core/plot-orchestrator.*` | 串口打开、`probe_open_grbl`、调用 `export_paths_to_grbl`、进度与取消 |
| 流水线 | `src/axidraw/pipeline/grbl-export.*` | 几何→G-code：`build_grbl_plot_gcode_string`、`export_paths_to_grbl`、预览 PathVector、统计 `GrblPlotStats` |
| 设备 | `src/axidraw/device/` | `grbl-client.*`（探测、`ok`、绘图期间泵主循环）、`grbl-link.*`（串口/TCP 统一发送）、`serial-port.*` / `tcp-port.*` |
| GUI 动作 | `src/actions/actions-axidraw.cpp` | `app.axidraw-plot`、`app.axidraw-export-gcode` |
| 应用 CLI | `src/inkscape-application.cpp` | `--export-grbl-gcode` → `export_grbl_gcode_document()` |
| 控制面板 | `src/ui/dialog/grbl-control-panel.*` 及 `grbl-panel-*.*`、`grbl-runtime-state.*` 等 | 连接、固件同步、填充预览、编辑器发送、运行时状态 |
| 菜单 | `share/ui/menus.ui` | 将上述 action 挂到「绘图机」相关菜单项 |
| 固件/对照仓库路径（维护者登记） | `src/axidraw/FIRMWARE.md` | 本地 `Grbl_Esp32`、`kxnx` 等路径，改协议时对照 |

## 2. 用户可见入口

### 2.1 GUI

- **发送文档到绘图机**：action `app.axidraw-plot` → `PlotOrchestrator::run_plot_grbl()` → `export_paths_to_grbl()`。
- **导出绘图机 G-code 到文件**：action `app.axidraw-export-gcode` → `build_grbl_plot_gcode_string()` → 写入用户所选路径。
- **绘图机工作台（控制面板）**：对话框类型 `GrblControl`（见 `src/ui/dialog/dialog-container.cpp`），内部可走「从图稿填充 G-code / 直接发送 / 编辑器逐行发送」等路径；填充与发送语义应与首选项及 `grbl-export.h` 一致。

动作名称与说明见 `src/actions/actions-axidraw.cpp` 中 `raw_data_axidraw`。

### 2.2 CLI（无界面导出）

- 选项：`--export-grbl-gcode`（定义于 `src/inkscape-application.cpp`）。
- 与 `--export-filename` 配合：实现见 `export_grbl_gcode_document()`；输出为 `-` 时写入 **stdout**，否则写文件；CLI 路径设置 `ctx.inhibit_interactive_pen_changes = true`，避免层间人工换笔对话框。
- 几何与参数仍来自当前 GRBL 首选项：`grbl_export_params_from_preferences()`。

## 3. 数据与控制流（精简）

```
文档 + 选择集 / 当前层
        │
        ▼
GrblExportParams  ←  grbl_export_params_from_preferences(/options/grbl/...)
GrblExportContext ←  desktop、selection、limit-to-current-layer、cancel、回调…
        │
        ├─► build_grbl_plot_gcode_string     （字符串，预览 / 导出文件 / CLI）
        │
        └─► export_paths_to_grbl(SerialPort)  （串流发送，逐行等待 ok）
```

- **同一套参数**：菜单直发、面板「从图稿填充」与导出文件应尽量一致；`plot-orchestrator.cpp` 顶部注释说明了与面板预览对齐的意图。
- **串口发送路径**：`export_paths_to_grbl` 内部通过设备抽象写入一行并等待控制器确认（见 `grbl-client` / `grbl-link`）。
- **预览**：`build_grbl_plot_preview_pathvector` / `build_grbl_plot_machine_preview_pathvector_in_doc_space` 用于画布叠加，几何阶段与导出对齐但不经串口。

## 4. 设备层要点

- **探测**：`probe_open_grbl` 判断端口上是否为 GRBL 应答（菜单直发在连接失败时会给出可读错误信息）。
- **绘图等待**：`grbl_begin_plot_waits` / `grbl_end_plot_waits` 与取消标志配合，发送期间可泵 UI（见 `plot-orchestrator.cpp` 中用法）。
- **笔控模式**：首选项 `pen-control` 为 `z`（使用 `pen-up-cmd` / `pen-down-cmd`）或 `m3m5`（固定 M5/M3 语义）；详见 `grbl-export.h` 文件头注释。

## 5. 控制面板模块拆分（便于定向修改）

`GrblControlPanel` 周边已将职责拆到多个单元（连接态、传输态、运行时相、固件同步、导出会话、发送器等）。修改发送/连接/同步行为时，优先在对应 `grbl-panel-*` 与 `grbl-runtime-state.*` 中查找，而不是只在 `grbl-control-panel.cpp` 单文件中堆砌逻辑。

## 6. 测试

- GRBL 相关用例注册在 `testfiles/CMakeLists.txt`（`grbl-*-test.cpp` 等）。
- 修改导出或协议行为时，应运行 targeted 测试（例如仓库惯例中的 `ninja check` 或项目约定的子集），并关注是否需为新分支补充 `grbl-export` / `grbl-client` 级别测试。

## 7. 参数键对照

- `/options/grbl/*` 与 `GrblExportParams` 的字段映射、默认值、范围及互斥关系，见 `doc/grbl-preferences-reference.md`。

## 8. 下一步文档缺口（未在本页展开）

- 控制面板操作流程（开发视角）与典型故障对照 `grbl_error_to_user_message` 的排障说明。

更多通用编译与贡献说明见 `doc/building/readme.md` 与仓库根目录 `CONTRIBUTING.md`。
