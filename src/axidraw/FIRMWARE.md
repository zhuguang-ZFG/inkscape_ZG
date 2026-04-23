# GRBL / 主机逆向参考（已固定记录）

Inkscape 树内 `src/axidraw` 与 GRBL 控制面板相关实现，参考源分三类：
- **设备侧固件行为**：`inkscape-axidraw` 仓库中的 `Grbl_Esp32`
- **主机侧逆向行为**：`kxnx` 逆向源码
- **历史/分支实现对照**：`inkscape-axidraw` 与 `inkscape_px`

## 本机已登记的参考路径（请勿在对话中重复询问）

| 说明 | 路径 |
|------|------|
| 固件源码（设备侧） | `D:\GIT\inkscape-axidraw\Grbl_Esp32` |
| 逆向源码（主机侧） | `D:\GIT\kxnx` |
| 参考仓库（主机/扩展实现） | `D:\GIT\inkscape-axidraw` |
| 参考仓库（分支实现） | `D:\GIT\inkscape_px` |

相对关系：
- 与本 Inkscape 仓库同级的 `inkscape-axidraw` 检出，子目录 `Grbl_Esp32`
- 与本 Inkscape 仓库同级的 `kxnx` 检出（奎享雕刻主机逆向源码）
- 与本 Inkscape 仓库同级的 `inkscape-axidraw` 检出（整仓对照）
- 与本 Inkscape 仓库同级的 `inkscape_px` 检出（分支/变体对照）

审阅时常见文件示例（相对上述 `Grbl_Esp32` 根）：

- `src/Config.h`
- `Machines/custom_3axis_hr4988.h`
- `Custom/paixi_writer_tool_change.cpp`
- `Spindles/NullSpindle.cpp`

后续改协议、笔控（Z vs M3/M5）、层间暂停、或主机交互语义时，优先对照以上已登记路径，并在代码注释里引用具体路径而非口头重复。
