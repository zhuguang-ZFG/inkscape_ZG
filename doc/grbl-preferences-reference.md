# GRBL 首选项键参考（`/options/grbl/*`）

本页对照 `src/axidraw/pipeline/grbl-export.cpp` 中 `grbl_export_params_from_preferences()` 的实际读取逻辑，整理：

- 首选项 key
- 对应 `GrblExportParams` 字段
- 默认值与限制范围（若有）
- 关键互斥/联动行为

> 说明：表中“默认值”来自 `getBool/getString/getDoubleLimited/getIntLimited` 的默认参数，不代表 UI 一定展示为该值。

## 1. 基础几何与速度

| Key | 字段 | 默认值 | 限制范围/备注 |
|-----|------|--------|---------------|
| `/options/grbl/flatness` | `flatness` | `0.08` | `[0.001, 10.0]` |
| `/options/grbl/feed-draw-mmmin` | `feed_draw_mm_min` | `1200.0` | `[60.0, 12000.0]` |
| `/options/grbl/feed-travel-mmmin` | `feed_travel_mm_min` | `6000.0` | `[60.0, 20000.0]` |

## 2. 笔控与延时

| Key | 字段 | 默认值 | 限制范围/备注 |
|-----|------|--------|---------------|
| `/options/grbl/pen-control` | （模式开关） | `z` | `m3m5`（大小写兼容 `M3M5`）时强制使用 `M5`/`M3 S1000` |
| `/options/grbl/pen-up-cmd` | `pen_up_cmd` | `G90\\nG1 Z0 F1200` | 仅 `pen-control=z` 生效 |
| `/options/grbl/pen-down-cmd` | `pen_down_cmd` | `G90\\nG1 Z5 F1200` | 仅 `pen-control=z` 生效 |
| `/options/grbl/enable-long-pen-up` | `enable_long_pen_up` | `false` | 仅 `pen-control=z` 生效；`m3m5` 下被强制 `false` |
| `/options/grbl/long-pen-up-mm` | `long_pen_up_mm` | `10.0` | `[-1000.0, 1000.0]` |
| `/options/grbl/long-move-dist-mm` | `long_move_distance_mm` | `20.0` | `[0.0, 100000.0]` |
| `/options/grbl/pen-up-delay-ms` | `pen_up_delay_ms` | `0.0` | `[0.0, 5000.0]` |
| `/options/grbl/pen-down-delay-ms` | `pen_down_delay_ms` | `0.0` | `[0.0, 5000.0]` |

## 3. 引导段、近连与稀疏采样

| Key | 字段 | 默认值 | 限制范围/备注 |
|-----|------|--------|---------------|
| `/options/grbl/enable-path-lead-in` | `enable_path_lead_in` | 继承旧键 | 默认回退到旧键 `/enable-path-lead-in-out` |
| `/options/grbl/path-lead-in-distance-mm` | `lead_in_distance_mm` | 继承旧键距离 | `[0.0, 1000.0]`；默认回退到旧键距离 |
| `/options/grbl/enable-path-lead-out` | `enable_path_lead_out` | 继承旧键 | 默认回退到旧键 `/enable-path-lead-in-out` |
| `/options/grbl/path-lead-out-distance-mm` | `lead_out_distance_mm` | 继承旧键距离 | `[0.0, 1000.0]`；默认回退到旧键距离 |
| `/options/grbl/enable-path-lead-in-out` | （旧键） | `false` | 仅作为新 lead in/out 默认回退来源 |
| `/options/grbl/path-lead-in-out-distance-mm` | （旧键） | `0.0` | `[0.0, 1000.0]`，仅回退来源 |
| `/options/grbl/enable-near-connect` | `enable_near_connect` | `false` | - |
| `/options/grbl/near-connect-distance-mm` | `near_connect_distance_mm` | `0.3` | `[0.0, 1000.0]` |
| `/options/grbl/enable-sparse-stroke-sampling` | `enable_sparse_stroke_sampling` | `false` | - |
| `/options/grbl/sparse-keep-every` | `sparse_keep_every` | `1` | `[1, 64]` |
| `/options/grbl/sparse-strategy` | `sparse_sampling_strategy` | `legacy` | `legacy` → `Legacy`；其他值一律 `Directional` |

## 4. 路径排序与填充

| Key | 字段 | 默认值 | 限制范围/备注 |
|-----|------|--------|---------------|
| `/options/grbl/optimize-stroke-order` | `optimize_stroke_order` | `true` | - |
| `/options/grbl/optimize-stroke-direction` | `optimize_stroke_direction` | `true` | - |
| `/options/grbl/contour-to-hatch` | `contour_to_hatch` | `false` | - |
| `/options/grbl/hatch-spacing-mm` | `hatch_spacing_mm` | `1.0` | `[0.05, 100.0]` |
| `/options/grbl/hatch-angle-deg` | `hatch_angle_deg` | `0.0` | `[-180.0, 180.0]` |
| `/options/grbl/hatch-cross` | `hatch_cross` | `false` | - |
| `/options/grbl/hatch-inset-enable` | `hatch_inset_enable` | `false` | - |
| `/options/grbl/hatch-inset-mm` | `hatch_inset_mm` | `0.1` | `[0.0, 100.0]` |
| `/options/grbl/hatch-angle-increment-enable` | `hatch_angle_increment_enable` | `false` | - |
| `/options/grbl/hatch-angle-increment-deg` | `hatch_angle_increment_deg` | `5.0` | `[-180.0, 180.0]` |

## 5. 坐标映射与床面裁剪

| Key | 字段 | 默认值 | 限制范围/备注 |
|-----|------|--------|---------------|
| `/options/grbl/flip-y-canvas` | `flip_y_canvas` | `false` | - |
| `/options/grbl/swap-xy` | `swap_xy` | `false` | - |
| `/options/grbl/invert-x` | `invert_x` | `false` | - |
| `/options/grbl/invert-y` | `invert_y` | `false` | - |
| `/options/grbl/align-content-min` | `align_content_min_to_origin` | `false` | - |
| `/options/grbl/clip-to-machine-bed` | `clip_to_machine_bed` | `true` | 迁移函数会在未设置时写入 `true` |
| `/options/grbl/machine-bed-width-mm` | `machine_bed_width_mm` | `210.0` | `[1.0, 2000.0]` |
| `/options/grbl/machine-bed-depth-mm` | `machine_bed_depth_mm` | `297.0` | `[1.0, 2000.0]` |

## 6. 分层暂停、手动换笔、M6

| Key | 字段 | 默认值 | 限制范围/备注 |
|-----|------|--------|---------------|
| `/options/grbl/auto-pause-between-layers` | `auto_pause_between_layers` | `false` | 当启用 `enable-layer-tool-change-m6` 时被强制为 `true` |
| `/options/grbl/manual-pen-change` | `manual_pen_change` | `false` | 实际值为 `auto_pause_between_layers && manual_pen_change`（且 M6 关闭时） |
| `/options/grbl/pen-change-to-home` | `pen_change_to_home` | `true` | - |
| `/options/grbl/pen-change-prompt` | `pen_change_prompt` | `true` | - |
| `/options/grbl/enable-layer-tool-change-m6` | `enable_layer_tool_change_m6` | `false` | `true` 时：`auto_pause_between_layers=true`、`manual_pen_change=false` |
| `/options/grbl/tool-change-use-point` | `tool_change_use_point` | `false` | - |
| `/options/grbl/tool-change-x-mm` | `tool_change_x_mm` | `0.0` | 使用 `getDouble`，未做 limited 夹紧 |
| `/options/grbl/tool-change-y-mm` | `tool_change_y_mm` | `0.0` | 使用 `getDouble`，未做 limited 夹紧 |
| `/options/grbl/auto-layer-pause-dwell-sec` | `auto_layer_pause_dwell_sec` | `0.0` | `[0.0, 600.0]` |

## 7. 头尾自定义 G-code

| Key | 字段 | 默认值 | 限制范围/备注 |
|-----|------|--------|---------------|
| `/options/grbl/start-gcode` | `start_gcode` | 空字符串 | 直接注入到起始段 |
| `/options/grbl/end-gcode` | `end_gcode` | `G0 X0 Y0` | 先 `normalize_end_gcode`；空白会回退默认值 |

## 8. 与迁移相关但不直接映射到 `GrblExportParams` 的 key

| Key | 用途 |
|-----|------|
| `/options/grbl/migrations/clip-to-machine-bed-default-v1` | 标记是否完成“床面裁剪默认开启”的一次性迁移 |
| `/options/grbl/migrations/end-gcode-default-v1` | 标记是否完成“空 end-gcode 回填默认值”的一次性迁移 |

## 9. 维护建议

- 新增或变更 `/options/grbl/*` 读取逻辑时，同时更新本页与 `doc/grbl-developer-overview.md`。
- 若修改了 key 的默认值/范围，建议补对应单测（优先 `grbl-export-test.cpp` 里偏参数行为的用例）。
