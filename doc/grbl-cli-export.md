# GRBL CLI 导出说明（`--export-grbl-gcode`）

本页说明如何通过 Inkscape 命令行导出原生 GRBL G-code，以及该流程和 GUI 导出的关系。

实现入口见 `src/inkscape-application.cpp`：

- 选项注册：`--export-grbl-gcode`
- 执行函数：`export_grbl_gcode_document(...)`

## 1. 行为摘要

- `--export-grbl-gcode` 会把可见矢量几何按当前 `/options/grbl/*` 首选项转换为 G-code。
- 导出核心使用 `build_grbl_plot_gcode_string(...)`，与 GUI“导出绘图机 G-code”走同一导出语义。
- CLI 路径会设置 `ctx.inhibit_interactive_pen_changes = true`，因此不会弹出层间手动换笔对话框。
- 当输出路径为 `-` 时，G-code 输出到 `stdout`，统计摘要输出到 `stderr`。

## 2. 常用命令示例

> 以下示例假设 `inkscape` 可执行文件已在 PATH 中；Windows 可替换为 `inkscape.exe` 的完整路径。

### 2.1 导出到文件

```bash
inkscape "input.svg" --export-grbl-gcode --export-filename="output.nc"
```

### 2.2 导出到标准输出（便于管道处理）

```bash
inkscape "input.svg" --export-grbl-gcode --export-filename=-
```

### 2.3 最小批处理示例（PowerShell）

```powershell
Get-ChildItem .\svgs\*.svg | ForEach-Object {
  $out = Join-Path .\gcode ($_.BaseName + ".nc")
  inkscape $_.FullName --export-grbl-gcode --export-filename=$out
}
```

## 3. 输出与失败语义

- 成功写文件时，CLI 会打印导出摘要（例如笔画数、范围、长度统计）。
- 写文件失败时，会输出类似：
  - `GRBL export failed to write '<path>': ...`
- 生成失败时，会输出类似：
  - `GRBL export failed: ...`

## 4. 与其他导出选项的关系

- `--export-grbl-gcode` 是独立导出路径，不依赖 `--export-type`。
- 通常应与 `--export-filename` 一起使用，便于明确输出位置或指定 `-`。
- 若未显式给出文件名，内部会按输入路径推导默认输出名（见 `derive_grbl_export_filename(...)`）。

## 5. 调试建议

- 先在 GUI 中确认 `/options/grbl/*` 参数，再跑 CLI，以减少“同图不同结果”误解。
- 出现“无可绘制路径”时，优先检查对象是否已转路径、可见性和图层选择策略。
- 批量任务建议先用 1 个样本文件验证输出，再全量执行。
