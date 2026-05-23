# ztp2mouse

`ztp2mouse` 是一个运行在 Android 用户态的小工具，用来把触控板的绝对坐标输入（ABS）转换成一个虚拟相对鼠标（REL）。

这个项目最初是为中兴键盘皮套触控板写的，目标是修复 Moonlight 开启 pointer capture 后原始触控板失效的问题。不过它的思路并不局限于这一台设备，只要你的 Android 触控板是 ABS 设备、而目标应用更适合捕获标准相对鼠标，这个项目就有参考价值。

## 项目作用

- 读取原始触控板输入事件
- 把绝对坐标位移转换成相对鼠标移动
- 通过 `uinput` 创建虚拟鼠标
- 输出 `REL_X`、`REL_Y`、`REL_WHEEL` 和鼠标按键事件
- 可选对源输入设备执行 `EVIOCGRAB`

## 功能特性

- 单指移动映射为鼠标移动
- 单指轻触映射为左键点击
- 双指轻触映射为右键点击
- 双指上下移动映射为滚轮
- 三指移动超过阈值后映射为左键拖动
- 如果硬件会上报 `BTN_MOUSE`，会按当前手指数映射左 / 右 / 中键
- 支持异常跳点过滤
- 支持灵敏度、滚轮缩放、轻触超时等参数调节
- 支持输入设备断开后的自动重连
- 支持守护模式
- 支持通过 Magisk 脚本自动启动 / 停止

## 为什么需要它

有些 Android 触控板在系统里暴露出来的是 ABS 指针设备，而不是标准的相对鼠标。

在 Moonlight 这类远程桌面 / 串流应用里，一旦开启 pointer capture，系统原本的触控板输入链路可能会被禁用，结果就是键盘皮套上的触控板直接失效。

`ztp2mouse` 的做法很直接：不改内核驱动，只在用户态读取原始触控板事件，再创建一个标准的 REL 鼠标给系统和应用使用。这样被捕获的是虚拟鼠标，而不是原始 ABS 触控板。

## 仓库内容

- `ztp2mouse.c`：核心程序
- `Android.mk`：NDK 构建入口
- `Application.mk`：ABI / platform 配置
- `build_ndk.ps1`：Windows 下的构建脚本
- `service.sh`：Magisk 服务脚本入口
- `mode-sync.sh`：按模式启动 / 停止 `ztp2mouse`
- `module.prop`：Magisk 模块元数据
- `customize.sh` / `uninstall.sh`：模块安装 / 卸载钩子
- `libs/arm64-v8a/ztp2mouse`：编译产物输出位置

## 运行前提

- 设备存在 `/dev/uinput`
- 设备已 root
- 能访问原始输入设备节点，例如 `/dev/input/event8`
- 如果要自行编译，需要 Android NDK

## 构建方法

Windows PowerShell 下直接执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_ndk.ps1
```

编译产物输出到：

```text
libs/arm64-v8a/ztp2mouse
```

如果你不想用脚本，也可以直接用 `ndk-build`，这是一个标准的 `Android.mk` 工程。

## 快速使用

先把二进制推到设备：

```powershell
adb push .\libs\arm64-v8a\ztp2mouse /data/local/tmp/ztp2mouse
adb shell chmod 755 /data/local/tmp/ztp2mouse
```

前台运行并输出详细日志：

```powershell
adb shell su -c "/data/local/tmp/ztp2mouse -i /dev/input/event8 -v"
```

后台守护运行：

```powershell
adb shell su -c "/data/local/tmp/ztp2mouse -i /dev/input/event8 -d"
```

调试时如果不希望独占抓取源设备，可以加 `-G`：

```powershell
adb shell su -c "/data/local/tmp/ztp2mouse -i /dev/input/event8 -G -v"
```

## 手势映射

- 单指移动：鼠标移动
- 单指轻触：左键
- 双指轻触：右键
- 双指上下移动：滚轮
- 三指移动超过阈值：左键拖动

补充说明：

- 当前源码不会把三指轻触当作中键点击
- 如果源设备支持 `BTN_MOUSE` 物理按压，按键会按当前手指数映射：
  单指为左键，双指为右键，三指及以上为中键

## 命令行参数

```text
-i <path>    输入设备路径
-u <path>    uinput 路径，默认 /dev/uinput
-n <name>    虚拟鼠标名称，默认 ztp_virtual_mouse
-s <value>   鼠标灵敏度，默认 1.35
-w <value>   双指滚轮缩放，默认 40.0
-t <ms>      轻触判定超时，默认 180
-m <px>      轻触允许的最大移动，默认 14
-j <px>      跳点过滤阈值，默认 200
-T <ms>      预留参数，当前版本未实际使用，默认 300
-D <px>      拖动起始阈值，默认 18
-G           不对源设备执行 EVIOCGRAB
-d           守护模式
-v           输出详细日志
```

## Magisk 模块说明

仓库里附带了一套 Magisk 模块脚本，但这里要说清楚：这部分更偏“作者自己的设备适配胶水”，不是一套可以直接通用到所有设备的模块方案。

当前脚本逻辑大致是：

- `service.sh` 监听键盘热键事件
- `mode-sync.sh` 根据当前桌面 / 模式决定是否启动 `ztp2mouse`

如果你要在别的设备上复用这套模块脚本，通常需要改这些内容：

- 目标包名
- 输入设备名
- 热键码
- 模块目录结构

相比之下，核心二进制 `ztp2mouse` 本身更容易复用。

## 调试建议

列出输入设备：

```powershell
adb shell su -c "getevent -lp"
```

检查虚拟鼠标是否创建成功：

```powershell
adb shell su -c "getevent -pl | grep ztp_virtual_mouse"
```

直接观察详细日志：

```powershell
adb shell su -c "/data/local/tmp/ztp2mouse -i /dev/input/event8 -v"
```

## 已知限制

- 这不是一个通用触控板驱动
- 行为高度依赖原始输入设备的事件时序
- 手感、灵敏度和手势稳定性通常需要按设备单独调参
- 当前 `-T` 参数虽然保留，但源码里还没有实际参与手势判定
- 某些瞬移、跳点、基准重置问题也可能来自原始驱动本身
- 仓库中的 Magisk 脚本是设备定制逻辑，不适合作为通用发布方案直接照搬

## 适用场景

- 修复 Moonlight pointer capture 场景下的键盘皮套触控板失效
- 在不改内核驱动的前提下，把 Android ABS 触控板桥接成 REL 鼠标
- 做触控板到鼠标映射的用户态实验

## English Summary

`ztp2mouse` converts an Android ABS touchpad into a virtual REL mouse through `uinput`. It was primarily written to make a keyboard-cover touchpad usable again in Moonlight pointer-capture mode, but the core idea can also be reused for other Android devices with similar input behavior.
