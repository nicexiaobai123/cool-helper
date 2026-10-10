# DwmCfgOverlayLab

实验性 x64 DLL，在已验证的 DWM D3D11 上屏路径中绘制无窗口 Dear ImGui 覆盖层。

参考：[看雪原帖](https://bbs.kanxue.com/thread-283488.htm)。

## 构建

```powershell
.\build.ps1                 # 默认 Release x64，优先 VS2019（v142），其次 VS2022
.\build.ps1 -Configuration Debug
```

构建成功后，脚本将 DLL 和 PDB 复制到对应的
`CoolHelperHub\build\<Configuration>` 目录，方便中台加载。
中台构建目录不存在时会提示并跳过复制，不视为构建失败。

## 整体架构

系统适配、Hook、纹理获取、显示策略和 UI 分层，避免扩展 Windows 版本时重复修改渲染逻辑。

```text
Bootstrap/Runtime
  → Bootstrap/InstanceCoordinator（同一 DWM 进程的实例协调）
  → Platform/SystemProbe（系统与模块信息）
  → Profiles/DwmHookProfiles（版本、环境、特征码及参数约定）
  → Scanner/PatternScanner（扫描与调用点校验）
  → Hooks/HookManager → AsmHook.asm（安装、转发与恢复）
  → Render/FrameRouter
      → DxgiSurfaceAdapter / DDisplaySurfaceAdapter
      → FrameTarget → OverlayRenderer（公共显示策略）
                    → RenderSession（每个显示器/设备的渲染状态）
  → UI/OverlayUi ↔ IPC/UiState
  → IPC/AnswerIpc（答案流）/ IPC/ControlIpc（控制指令）
```

- `Profiles`：维护系统版本范围、环境判断、特征码变体、参数寄存器和上屏类型。
- `InstanceCoordinator`：同一 DWM 进程只允许一个实例安装 Hook；后加载的实例
  只转发 `ShutdownDwmOverlay`，不重复安装。
- `HookManager`：管理 CFG/fothk 调用点补丁、邻近分派单元、可执行跳板、命中路由和卸载状态。
- `AsmHook.asm`：保存通用寄存器上下文；Win10 跳转到原虚函数目标，Win11
  经邻近跳板及原有 fothk/XFG 分派路径返回。
- `FrameRouter` 与适配器：把纹理、显示器身份和可选的绘制前后操作归一化为
  `FrameTarget`。DDisplay 的私有 ABI 仅保留在适配器中，借用的参数和原生对象不逃逸出同步回调。
- `DisplayTopology`：解析显示器身份；`OverlayRenderer`：应用公共显示策略，管理数量受限的会话。
- `RenderSession`：独立持有 D3D11、ImGui、字体、布局和背景状态；`OverlayUi`
  不依赖 Windows 版本、Hook 或 IPC 传输方式。
- `UiStateStore` 和 `OverlayCommandQueue`：提供容量受限的状态/命令存储，减少 Present 路径的等待。

### 答案与控制通信

`AnswerIpcService` 在独立工作线程读取中台 `OverlayIpcSink` 写入的无锁 SPSC
环形队列，命名共享内存为 `Local\CoolHelper.Overlay.Answer.v1`。双方维护心跳，
中台重启后自动重连。答案协议分别定义在 `IPC/AnswerIpc.h` 和中台
`include/coolhelper/OverlayIpc.h`，修改时必须保持字节布局一致。

只有成功安装 Hook 的主实例才启动通信工作线程。Present 线程通过 SRWLock
复制答案快照，在各目标显示器上展示相同内容。UI 使用深色半透明样式，支持标题、
粗体、行内代码、连续代码块、嵌套列表、引用、表格、链接和分隔线等 Markdown 子集，
并支持流式自动滚动。

控制通道为 `Local\CoolHelper.Overlay.Control.v1`，传递隐藏/显示、答案滚动等低频指令。
协议统一定义在 `../Shared/OverlayControlProtocol.h`。DLL 回报实际显示状态和心跳，
中台依据实际状态切换覆盖层。v1 保留字段用于存储显示目标配置及能力/同步回报，
不改变原有大小和命令布局；重连会重新应用设置，旧 DLL 会显示“不支持显示策略”。

覆盖层从隐藏切换为显示时，DLL 的重绘工作线程异步请求一次桌面重绘，
避免中台进入托盘、桌面静止时等待鼠标活动才出现。连续请求会合并；不弹出中台、
不模拟输入、不同步强制绘制，真正的覆盖层绘制仍在原 Present 回调中完成。
日志 `Overlay show: desktop repaint requested asynchronously` 表示请求已发出，
不代表该帧已完成上屏。

### 显示策略与多屏状态

中台提供“兼容默认 / 仅主屏 / 所有屏幕”，对应 `Compatible / PrimaryOnly / AllDisplays`。
`Bootstrap` 设置兼容默认值：Win10 不限制屏幕，Win11 仅主屏；适配器不负责决定画在哪块屏幕。

显式选择主屏或所有屏幕时，要求显示器身份已确认、目标未旋转且尺寸匹配。
身份未知的 Win10 旧式交换链仅允许在“兼容默认”下保留原有绘制行为，不按分辨率猜测归属。

最多维护 8 个显示器/设备会话，每个会话独立持有 ImGui 上下文、DPI 字体、布局和背景副本，
最多缓存 8 组交换链/资源背景。已确认的 DXGI 翻转缓冲区使用代际标识，
即使同尺寸调用 `ResizeBuffers` 也会淘汰旧副本。回调之间不保留原始 `GetBuffer` 纹理引用。
无法确认原生资源代际时不做猜测；缓存达到上限后安全跳过绘制。

每个会话独立消费相同的累计滚动指令。鼠标和重绘区域按显示器桌面原点换算，支持负坐标。
隐藏、切换策略和关闭运行时会清理各显示器上已绘制的区域。

### 背景恢复与隐藏清理

`BackdropCompositor` 在 GPU 上比较当前像素与上次覆盖层最终输出：仍相同的像素保留
干净背景，已变化的像素采用 DWM 新合成内容。恢复背景后，再由 ImGui 进行透明混合，
并异步请求桌面重新合成，避免直接使用过期截图造成残留或重影。

隐藏后不立即跳过 Present：每个曾绘制覆盖层的轮换缓冲区执行比较与背景恢复，不再画 UI，
清理完成后停止处理隐藏帧。新合成的桌面像素不会被旧背景覆盖。若隐藏与绘制同时发生，
会在当前缓冲区返回 Present 前执行清理。

原生脏区域准备和 GPU 刷新仍在 Present 线程执行，不从 IPC 工作线程访问 D3D11。
重绘工作线程为隐藏/被排除的目标请求背景擦除，包括桌面子窗口；正常帧的重绘不取消
已有的擦除请求，也不使用可能阻塞其他进程的同步重绘。

## 扩展系统版本或上屏路径

1. 在 `Profiles/DwmHookProfiles.cpp` 增加 `PatternVariant`。
2. 增加 `HookSpec`，明确系统/模块版本范围、环境条件和已验证的参数来源。
   累积更新小版本仅在函数和调用点语义匹配时共用配置。
3. 参数不是兼容 DXGI 的交换链时，增加输出 `FrameTarget` 的适配器，并在
   `Render/FrameRouter.cpp` 注册。私有 ABI、纹理提取和绘制前后操作留在适配器中，
   不向公共渲染层增加版本专用 UI 或显示策略。
4. 验证特征码唯一，选中的指令确为预期的 `FF 15 disp32` CFG 调用、
   进入 `fothk` 的 `E8 rel32`，或拥有专用适配器的已确认本地调用。
5. 记录系统版本、`dwmcore.dll` 文件版本、物理/虚拟 GPU 和运行时命中次数。

不支持的版本拒绝安装。未确认参数约定前，不要使用“找到第一个 `FF 15` 就安装”等宽泛回退。

## 系统兼容情况

- Windows 10 1909 / 18363：VMware 3D（`vm3dum64*.dll`），已验证
  `dwmcore.dll 10.0.18362.752`。
- Windows 10 22H2 / 19045：已验证物理机。
- Windows 11：保留调试器确认过的内联 Legacy 配置，范围为系统 `26100–26200`、
  dwmcore 文件构建号 `26100`，要求函数/调用点特征码唯一，RDX 指向
  `dxgi!CDXGISwapChainDWMLegacy`，保留原有 fothk 分派。此项不代表支持所有 Win11。
  未命中的拆分 Legacy 候选及临时探针已移除。

### Win11 DDisplay 上屏路径

已确认系统 26200、`dwmcore.dll 10.0.26100.9278`：
`SizeOfImage=0x443000`、Stamp=`0x9A1AF3BA`、CheckSum=`0x004477E9`。
该映像只安装一个 DDisplay 绘制 Hook，不安装未执行的 Legacy 候选。
其他匹配映像仍可使用独立的内联 Legacy 配置，临时路径探测代码已删除。

```text
CDDisplaySwapChain::PresentMPO（RVA 140910）
  → Hook ExecutePresent 调用点（RVA 1409F9）
  → 从 R8 平面数组 / R9D 数量中选择启用的基础平面 0
  → CDDisplaySwapChainBuffer::GetD3D11Resource
  → QueryInterface(ID3D11Texture2D)
  → FrameTarget → 公共显示策略 → 每屏 RenderSession
  → 原始 CDDisplaySwapChain::ExecutePresent（RVA 2BD2C4）
```

`Profiles/Win11DDisplayProfile.h` 负责映像与特征码选择；
`Render/DDisplaySurfaceAdapter` 负责已验证的私有 ABI 和资源提取，
不把原生缓冲区强制转换为 `IDXGISwapChain`。

关键 ABI 校验依据：

- `CDDisplaySwapChainBuffer` 虚表 RVA `300E48`，槽位 `+98h` 指向
  `GetD3D11Resource`（`1F9E90`，返回借用资源）。
- scanout 查询 IID `AAC1AA85-B883-5C29-B7C1-C2EAAEB3DA75`；
  `ddisplay.dll` 虚表 `43ED8`、槽位 `+30h` 指向 `SetPlaneDirtyRects`（`293A0`），
  矩形使用 X/Y/Width/Height。绘制前标记整帧脏区域，覆盖 DWM 原有更新范围，
  不能仅标记 UI 区域而遗漏其他桌面更新。
- 额外校验 `ddisplay.dll` 映像大小 `60000`、Stamp=`A9FD047A`、实际对象虚表和方法入口字节。
  未知布局跳过绘制，未经重新确认 ABI 不放宽版本条件。
- 交换链接口（`this +18h`、虚表 `3084A0`）提供适配器 LUID 和 VidPn target ID，
  已验证的取值函数为 `1F1080 / 2BDBA0`。通过
  [QueryDisplayConfig](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig)
  映射到活动显示路径和 GDI 显示器，不以相同分辨率判断身份。

显示拓扑缓存 2 秒，并发刷新调用跳过等待。“兼容默认”仅画主屏，
“所有屏幕”允许每个已验证且受支持的目标使用独立会话。
未知映射、旋转/缩放平面、立体、MSAA 或受保护表面跳过并记录原因。
支持 BGRA8/RGBA8/RGB10A2/RGBA16F。

背景按交换链/资源隔离，尺寸、格式或桌面原点变化时重建会话；背景保存失败则不绘制。
GPU 写入在原始 `ExecutePresent` 前刷新。整帧脏区域优先保证正确性，暂不优化局部呈现带宽。
显示器热插拔、主屏切换和硬件 MPO 切换仍需目标机器验证，不代表任意多屏布局都已验证。

更新时先卸载旧 DLL，再注入新 DLL。可通过 DebugView 查看路径选择、Hook 安装及命中、
纹理适配器就绪、渲染会话建立和 ImGui 初始化日志。隐藏时会报告缓冲区背景恢复及
整体清理完成，关闭时会打印命中统计，在恢复 Hook 并等待回调结束后释放渲染资源。

## 隔离测试

以下测试仅使用模拟模块、合成纹理和 WARP，不注入 DWM，也不构建正式 DLL：

```powershell
cmake -S tests -B ../analyze_result/win11_legacy_hook_tests_build -G "Visual Studio 16 2019" -A x64
cmake --build ../analyze_result/win11_legacy_hook_tests_build --config Release
ctest --test-dir ../analyze_result/win11_legacy_hook_tests_build -C Release --output-on-failure
```

覆盖 Legacy 回归、DDisplay 回调/寄存器约定、Hook 互斥选择、映像/被调函数校验、
九参数转发与精确恢复、原生平面读取边界、纹理 QI 引用归属、脏区域 ABI、
同分辨率显示器身份、四种格式的背景恢复/GPU 回读及管线状态恢复。

同时验证独立 WARP 设备/ImGui 上下文、负桌面坐标、多屏隐藏、轮换缓冲区清理、
隐藏与绘制竞争、新桌面像素保留，以及重复同尺寸 `ResizeBuffers`。
公共渲染和路由源码也会进行编译检查，Win10 Hook 配置不变。

中台测试覆盖设置迁移、显示目标同步、重连和旧版控制协议兼容，使用独立测试对象名称。
这些测试不能替代物理机或 VMware 验收；Win10/Win11 主屏/所有屏幕选择、不同 DPI、
热插拔、残影、隐藏/显示和卸载仍需实际测试。

## 生命周期与安全卸载

通常由中台使用 `CreateRemoteThread + LoadLibraryW` 加载 DLL。
完整卸载必须先调用 `ShutdownDwmOverlay`，再调用 `FreeLibrary`，最后确认模块已移除。

`ShutdownDwmOverlay` 停止通信工作线程、恢复调用点补丁、等待活动回调结束，
释放 ImGui/D3D11 资源及分派单元。`DLL_PROCESS_DETACH` 不在加载器锁下执行清理。

覆盖层只是显示界面，生命周期由外部控制器管理；隐藏并不卸载 Hook、IPC 或渲染资源。
`ShutdownDwmOverlay` 也不会减少 Windows 加载器引用计数，控制器仍需调用 `FreeLibrary`。
若为调用导出函数额外加载了临时 DLL 实例，也必须释放该实例。
手动映射的映像由映射器移除，不能用 `FreeLibrary` 卸载。
