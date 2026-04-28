# GRBL 控制面板流程（开发视角）

本页描述 `src/ui/dialog/grbl-control-panel.*` 的主流程，帮助在改连接、发送、固件同步或 UI 状态时快速定位入口与状态切换点。

## 1. 主体结构与分层

`GrblControlPanel` 已从“单文件大逻辑”拆分为“UI 组装 + 状态/流程子模块”：

- 连接生命周期：`grbl-panel-connection-state.*`
- 传输活动状态：`grbl-panel-transport-state.*`
- 发送执行器：`grbl-panel-sender.*`
- 导出会话：`grbl-panel-export-session.*`
- 固件同步：`grbl-panel-firmware-sync.*` 与 `grbl-panel-firmware-sync-state.*`
- 运行时阶段：`grbl-runtime-state.*`
- 入口 guard：`grbl-panel-job-guard.*`

优先修改对应子模块；只有在组合调度层面才回到 `grbl-control-panel.cpp`。

## 2. 连接与断开流程

关键入口：

- `connect_toggle()`
- `resolve_connect_request(...)`
- `start_connect_worker(...)`
- `finish_connect_attempt_*_ui(...)`
- `disconnect_controller(...)`

状态计划器（建议优先看）：

- `make_connect_attempt_begin_plan()`
- `make_connect_attempt_success_plan(...)`
- `make_connect_attempt_failure_plan(...)`
- `make_disconnect_controller_plan()`

这些 plan 通过 `apply_connection_plan(...)` 落到 UI/轮询/状态标签，避免在入口函数里直接拼状态分支。

## 3. 固件同步流程

关键入口：

- `request_firmware_sync(...)`
- `begin_firmware_sync()`
- `launch_firmware_sync_worker()`
- `complete_firmware_sync_ui(...)`

固件同步期间传输状态由 transport plan 控制：

- `make_transport_plan_for_firmware_sync_start(...)`
- `make_transport_plan_for_firmware_sync_complete(...)`

典型行为是：开始时暂停机床状态轮询，结束后按连接状态恢复。

## 4. 三条发送路径

### 4.1 从图稿填充编辑器

入口：`on_fill_gcode_from_document()`

核心是构造导出上下文与参数，把图稿转成文本 G-code（不触发串流）。

### 4.2 从图稿直接发送（串流）

入口：`on_send_document_direct()`

调用链：

1. guard：`get_grbl_direct_send_block_reason(...)`
2. 线程/运行时：`begin_gcode_stream_ui(...)` + `run_gcode_stream_thread(...)`
3. 发送器：`GrblPanelSender::run_direct_send_worker(...)`
4. 导出串流：`export_paths_to_grbl(...)`

### 4.3 发送编辑器中的 G-code

入口：`on_send_gcode()`

调用链：

1. guard：`get_grbl_editor_send_block_reason(...)`
2. 线程/运行时：`begin_gcode_stream_ui(...)` + `run_gcode_stream_thread(...)`
3. 发送器：`GrblPanelSender::run_editor_gcode_send_worker(...)`
4. 单行发送：`GrblLink::send_line_wait_ok(...)`

## 5. 取消与收尾

关键入口：

- `on_cancel_gcode_stream()`
- `request_gcode_cancel_ui(...)`
- `finish_gcode_stream_from_worker()`
- `complete_gcode_stream_ui(...)`

设计要点：

- 取消标志由 `_gcode_cancel` 管理，worker 与 UI 都能安全观察。
- 收尾统一走 `finish_*`/`complete_*` 路径，保证按钮状态、轮询恢复和线程 join 顺序一致。

## 6. 预览与反馈刷新

关键入口：

- `request_plot_feedback_for_trigger(...)`
- `schedule_plot_feedback_refresh(...)`
- `refresh_plot_feedback(...)`
- `sync_plot_preview_overlay()`

常见触发：映射参数变化、编辑器 G-code 变化、发送完成后刷新摘要与预览。

## 7. 修改建议（避免回归）

1. **先找 plan 函数**：连接/传输状态优先改 `make_*_plan(...)`，避免分支散落。
2. **发送入口先过 guard**：新增发送模式时先定义阻断条件与测试。
3. **保持 finish 路径单一**：不要在多个线程分支里重复改 UI 控件状态。
4. **先补最小测试**：优先在 `grbl-panel-*-test.cpp` 补状态迁移与 guard 优先级。
