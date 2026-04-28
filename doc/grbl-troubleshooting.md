# GRBL 常见错误与排障对照（主机侧）

本页面向本仓库的 GRBL 主机端流程（菜单直发、控制面板发送、CLI 导出），把常见报错映射到可执行排查动作。

## 1. 快速定位：错误来源在哪

- **菜单直发 / 控制面板直发**：核心报错来自 `grbl-client.cpp` 的 `grbl_error_to_user_message(...)` 及连接探测分支。
- **控制面板编辑器发送**：通过 `grbl-panel-sender.cpp` 与 `grbl-link.cpp` 返回错误（如 `not connected`、`blocked during streaming`、超行数）。
- **CLI 导出**：`src/inkscape-application.cpp` 的 `export_grbl_gcode_document(...)`，主要是“生成失败”或“写文件失败”。

## 2. 常见错误 -> 含义 -> 建议动作

| 用户可见/原始错误 | 含义（主机侧） | 建议动作（按优先级） |
|---|---|---|
| `Operation cancelled.` | 用户取消或 UI cancel flag 生效（`grbl_error_user_cancelled()`） | 1) 这不是故障；2) 若频繁误触，检查发送前后的取消状态流转 |
| `Could not write to the serial port...` / `serial write failed` | 主机写串口失败 | 1) 检查串口号/占用；2) 校验线缆与供电；3) 复核波特率与控制器型号配置 |
| `Timed out waiting for a response from the controller...` / `timeout waiting for controller response` | 已发送但长时间未收到 `ok` | 1) 检查控制器是否报警/暂停；2) 核对波特率；3) 看是否连接不稳定导致掉线 |
| `Unexpected response from the controller: ...` / `unexpected response: ...` | 返回了非 `ok`、非 `error:`、非可忽略噪声行 | 1) 记录原始回包；2) 检查是否有中间设备注入文本；3) 对照固件协议版本 |
| `The controller reported an error:\nerror:N` | 固件明确返回 `error:` | 1) 保留该错误号；2) 对照固件错误码表；3) 回看触发该行 G-code 与机床状态 |
| `Serial port is disconnected.` | 控制面板 direct send 前就检测到串口不可用 | 1) 先重连串口；2) 避免热插拔后直接继续发送；3) 重新 probe |
| `not connected` | `GrblLink` 未绑定可用连接 | 1) 在控制面板先建立连接；2) 检查 serial/tcp 状态是否已关闭 |
| `blocked during streaming` | `streaming` 期间发送了被阻断查询（如 `?/$I/$G/$#/$$`） | 1) 将状态查询移到非 streaming 窗口；2) 不要在发送循环中插入这些查询 |
| `Too many G-code lines (limit exceeded).` | 编辑器发送超过上限（当前 200000 行） | 1) 分段发送；2) 减少冗余行；3) 先做离线压缩/合并 |
| `GRBL export failed: ...`（CLI） | 几何生成或参数处理失败 | 1) 检查输入 SVG 是否存在可绘制路径；2) 检查层/选择策略；3) 先在 GUI 预览导出 |
| `GRBL export failed to write '<path>': ...`（CLI） | 输出文件写入失败 | 1) 检查路径权限与磁盘空间；2) 避免只读目录；3) 改用新文件名或 `--export-filename=-` 验证生成是否正常 |

## 3. 连接探测类问题（菜单直发常见）

### 3.1 无状态响应

典型提示：`No GRBL status response on <device> at <baud>...`

优先排查：

1. 串口路径是否正确（Windows 常见 `COMx`）。
2. 波特率是否与固件一致。
3. 控制器供电和 USB/串口驱动是否正常。

### 3.2 回应了但不像 GRBL

典型提示：`The selected serial port ... answered, but not like a GRBL controller:`

优先排查：

1. 端口是否连到了非 GRBL 设备。
2. 是否串口复用导致读到其他程序输出。
3. 记录回包文本并与目标固件协议对照。

## 4. 发送阶段排障顺序（推荐）

1. **连接状态**：先排除 `not connected`/`Serial port is disconnected.`。
2. **写入能力**：再排除 `serial write failed`。
3. **响应能力**：再排除 timeout 与 unexpected response。
4. **固件业务错误**：最后分析 `error:N` 与对应 G-code。

这样可以避免把“链路问题”误判为“刀路或参数问题”。

## 5. 需要保留的最小现场信息

出现间歇问题时，建议至少保留：

- 输入文件名（或最小复现 SVG）
- 触发路径（菜单直发 / 编辑器发送 / CLI）
- 串口参数（设备、波特率）
- 原始错误文本（完整一行）

若需更深排查，可结合工作目录下的 GRBL 主机写日志进行对照。
