# ZG 安装与恢复指南

本文用于在另一台 Windows 电脑上恢复当前的 Inkscape ZG 主程序与 Grbl_Esp32 固件环境。

## 1. 准备基础软件

先安装以下工具：

- Git
- MSYS2
- CMake
- Ninja
- Visual Studio Build Tools
- Python 3
- PlatformIO Core

建议把 `git`、`cmake`、`ninja`、`python`、`platformio` 加入系统 `PATH`。

## 2. 拉取主工程

在目标目录执行：

```bash
git clone --recurse-submodules https://github.com/zhuguang-ZFG/inkscape_ZG.git
cd inkscape_ZG
```

如果子模块没有完整拉下，再执行：

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

## 3. 编译 Inkscape ZG

在仓库根目录执行：

```bat
setup-zg-build-env.cmd
build-zg-inkscape.cmd
```

编译完成后，程序位于：

```text
inkscape\build-zg\install_dir\bin\inkscape.exe
```

启动命令：

```bat
inkscape\build-zg\install_dir\bin\inkscape.exe
```

## 4. 拉取固件仓库

单独拉取固件：

```bash
git clone https://github.com/zhuguang-ZFG/Grbl_Esp32.git
cd Grbl_Esp32
git checkout Branch_34206ccc
```

## 5. 编译与刷写固件

编译：

```bash
platformio run -e release
```

刷写到控制板：

```bash
platformio run -e release -t upload --upload-port COM3
```

如果串口不是 `COM3`，改成实际端口号。

## 6. 刷机后必须检查

串口连接控制器后执行：

```gcode
$$
```

确认至少以下参数正确：

```gcode
$1=255
$80=0.000
$81=5.000
$112=600.000
$122=60.000
```

如果不是这些值，就手动设置：

```gcode
$1=255
$80=0
$81=5
$112=600
$122=60
```

然后再次执行：

```gcode
$$
```

确认设置已经保存。

## 7. 当前关键行为说明

当前版本包含这些关键修正：

- `设置原点` 只设置 `X/Y`，不再改写 `Z`
- 默认 Z 抬笔/落笔命令使用绝对模式
- 默认 Z 抬落笔速度已降为更保守值
- 固件侧 Z 轴最大速度与加速度已调低
- 固件侧步进空闲保持改为持续上电，降低弹簧回弹影响

## 8. 首次验证建议

首次恢复后，不要立刻跑复杂图。

先测试简单 Z 动作：

```gcode
G90
G1 Z0 F300
G4 P0.2
G1 Z5 F300
G4 P0.3
G1 Z0 F300
G4 P0.2
G1 Z5 F300
```

再测试小范围绘图，确认：

- 落笔稳定
- 不会偶发压不到底
- 不会出现 Z 高度越画越飘

## 9. 仓库对应关系

主工程仓库：

```text
https://github.com/zhuguang-ZFG/inkscape_ZG
```

固件仓库：

```text
https://github.com/zhuguang-ZFG/Grbl_Esp32
```

当前固件使用分支：

```text
Branch_34206ccc
```

## 10. 常见问题

### 10.1 落笔有时不到位

优先检查：

- `$1` 是否为 `255`
- `$112` 是否为 `600`
- `$122` 是否为 `60`
- HR4988 的 Z 驱动电流是否过小
- Z 丝杆、联轴器、顶丝是否打滑
- 弹簧回位是否一致

### 10.2 刷机后参数没变

刷固件不会总是覆盖 EEPROM 里的旧设置，所以要手动执行 `$$` 检查。

### 10.3 子模块拉不下来

执行：

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

如果网络不稳定，可以多执行一次。

### 10.4 安装包启动时报缺少 DLL

如果双击安装后的 `inkscape.exe`，一上来就报：

- `找不到 lib2geom.dll`
- 或者连续提示还缺别的 DLL

不要先怀疑编译失败，先怀疑安装包 staging 漏文件。

这次已经确认过一个典型坑：

- `install_dir\bin` 里明明有 `lib2geom.dll`
- 但 NSIS 最终打包目录 `_CPack_Packages\win64\NSIS\...\inkscape\bin` 里没有
- 结果安装包装出来仍然会报缺 DLL

正确检查方式：

```powershell
Get-ChildItem inkscape\build-zg\_CPack_Packages\win64\NSIS\*\inkscape\bin\lib2geom.dll
Get-ChildItem inkscape\build-zg\_CPack_Packages\win64\NSIS\*\inkscape\bin\libinkscape_base.dll
Get-ChildItem inkscape\build-zg\_CPack_Packages\win64\NSIS\*\inkscape\bin\libgtk-4-1.dll
```

只看下面这个目录是不够的：

```text
inkscape\build-zg\install_dir\bin
```

因为它只是本机构建安装树，不等于最终安装包实际带走的文件。
