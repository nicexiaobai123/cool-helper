# CoolHelperHub

Windows x64 C++ 中台程序：截取主屏，将 PNG 转为 Base64，提交给兼容 OpenAI
接口的视觉模型，并在 Dear ImGui 界面和 DWM 覆盖层中展示流式答案。

## 构建

默认使用 Release x64 增量构建：

```powershell
.\build.cmd
```

常用选项：

```powershell
.\build.cmd -Configuration Debug     # 构建 Debug 版本
.\build.cmd -Clean                   # 清理后重新构建
.\build.cmd -Test                    # 构建并运行测试
.\build.cmd -Package                 # 打包到 out\ 下的时间戳目录
.\build.cmd -Run                     # 打包并运行独立副本
.\build.cmd -Help                    # 查看所有选项
```

`-Run` 运行带时间戳的独立副本，避免正在运行的程序锁住正常构建输出。
也可以直接使用 CMake：

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
ctest --preset release
```

程序输出到 `build/Release/CoolHelperHub.exe`。

## 使用与设置

在“设置”页配置 API Base URL、API Key、支持视觉输入的 Model，以及可选的
System Prompt 和截图问题。例如 API Base URL 可以是 `https://api.openai.com/v1`。

内置两套提示词预设：

- 综合面试：面向 C/C++、Windows 客户端和逆向岗位。
- 算法题：面向 LeetCode 类题目，提供思路、关键点、完整代码、复杂度、边界条件，以及可能追问的答案提示。

答案默认优先使用 C++17，尽量兼容 C++11，仅在题目要求时使用 C++20。
手动选择预设可以恢复提示词，不会自动覆盖自定义内容。

默认截图快捷键为 `Ctrl+Alt+S`，可在设置页修改为 Ctrl/Alt/Shift/Win
配合 A-Z、0-9 或 F1-F12。窗口按钮和托盘菜单也可触发主屏截图。
覆盖层默认使用 `Ctrl+Alt+H` 隐藏/显示，`Ctrl+Alt++` 向下滚动、
`Ctrl+Alt+-` 向上滚动；这些快捷键也可在设置页配置。

关闭主窗口只隐藏到托盘，不会中止正在进行的请求。托盘状态下，截图、答案
和错误都留在后台，不自动弹出窗口；右键托盘图标，选择“显示窗口”恢复界面。
再次启动程序也不会让已有托盘实例弹出。通过托盘“退出程序”结束中台。

设置保存在 `%LOCALAPPDATA%\CoolHelperHub\settings.json`，API Key 使用
当前 Windows 用户的 DPAPI 加密。

## DWM 覆盖层

“DWM 覆盖层”页管理 `dwm.exe` 中的 `DwmCfgOverlayLab.dll`：

- 注入 DLL：使用 `CreateRemoteThread + LoadLibraryW` 加载 DLL。默认路径为
  `CoolHelperHub.exe` 同目录的 `DwmCfgOverlayLab.dll`，修改后的路径会保存到设置。
- 卸载 DLL：先远程调用 `ShutdownDwmOverlay` 恢复 Hook、等待回调结束，再调用
  `FreeLibrary` 并检查模块是否已移除。恢复失败时保留 DLL，并显示原因。
- 运行状态：每秒刷新当前控制台会话的 DWM 进程、DLL 驻留和 IPC 连接状态。
- 显示目标：选择“兼容默认 / 仅主屏 / 所有屏幕”，立即保存，并在 DLL 连接或重连时同步。
  “兼容默认”保留 Win10 不限制屏幕、Win11 仅主屏的行为。显式选择主屏或所有屏幕时，
  只在身份已确认且受支持的目标上绘制；不通过分辨率猜测显示器。页面会显示同步状态，
  不支持显示策略的旧 DLL 需要更新。

Win10 DXGI 与 Win11 DDisplay 使用同一套显示策略。各显示器/设备独立维护
UI、字体和背景恢复状态，但接收相同的答案和滚动指令。隐藏、切换显示目标及
关闭运行时会清理已绘制区域；隐藏后还会在后续 Present 中恢复已污染的轮换缓冲区。

控制协议统一定义在 `../Shared/OverlayControlProtocol.h`，保持 v1 的原有
布局大小和隐藏/滚动命令兼容性。

注入前，中台检查 DLL 的 PE 导入，并尝试使用 DWM 账户令牌检查文件读取权限。
`dwm.exe` 以当前会话的 `DWM-x` 账户读取 DLL，用户目录、映射盘或虚拟机共享目录
可能无法访问；建议将 DLL 放在本地磁盘上、与 `CoolHelperHub.exe` 同目录。

程序通过 `resources/admin-uac.props`（`VS_PROJECT_IMPORT`）声明管理员权限，
启动时启用 `SeDebugPrivilege`，用于访问 `dwm.exe`。

## 架构与数据流

```text
Win32 / ImGui 中台
  → GDI 主屏截图 → WIC PNG → Base64
  → OpenAIClient（WinHTTP）→ SseParser
  → TeeAnswerSink
      ├→ QueuedAnswerSink → 本地 UI 事件队列
      └→ OverlayIpcSink → 共享内存环形队列 → DWM 覆盖层
DllInjector：FindDwmProcess / Inject / Eject
OverlayControlChannel → Shared/OverlayControlProtocol.h → 覆盖层控制
```

- 网络请求在后台线程执行，支持取消；新截图会取消旧请求。
- 每个 `AnswerEvent` 按顺序发送 UTF-8 Markdown。答案通道为
  `Local\CoolHelper.Overlay.Answer.v1`，采用无锁 SPSC 环形队列和命名事件；
  发送线程负责等待，`Publish` 不阻塞调用方。
- 双方通过心跳检测重启，DLL 自动重连；只处理当前请求的事件，防止取消后的旧答案混入。
- 日志写入 `%LOCALAPPDATA%\CoolHelperHub\logs`，不记录 API Key、截图、Base64、提示词或答案正文。
- Release 使用静态 MSVC 运行库，其余 DLL 依赖为 Windows 系统组件。
