# DwmCfgOverlayLab

An experimental x64 DLL that renders a handleless Dear ImGui layer into a
verified DWM D3D11 presentation path.

Reference: https://bbs.kanxue.com/thread-283488.htm

## Build

```powershell
.\build.ps1                 # Release|x64 via VS2019 (v142), falls back to VS2022
.\build.ps1 -Configuration Debug
```

After a successful x64 build the script copies `DwmCfgOverlayLab.dll` (and its
PDB) into the matching `CoolHelperHub\build\<Configuration>` directory so the
hub can inject it directly; a missing hub build directory is reported and
skipped, not an error.

## Architecture

The runtime is deliberately split so Windows compatibility work does not
change rendering or UI code:

```text
Bootstrap/Runtime
  -> Bootstrap/InstanceCoordinator
  -> Platform/SystemProbe
  -> Profiles/DwmHookProfiles
  -> Scanner/PatternScanner
  -> Hooks/HookManager -> AsmHook.asm
  -> Render/FrameRouter -> Render/OverlayRenderer
  -> UI/OverlayUi <-> IPC/UiState
  -> IPC/AnswerIpc (CoolHelperHub answer stream)
```

- `Profiles` owns build ranges, environment predicates, byte-pattern variants,
  argument registers, and presentation kinds.
- `InstanceCoordinator` elects one owner per DWM process. A later DLL image is
  control-only and forwards `ShutdownDwmOverlay` to the owner instead of
  installing a second hook chain.
- `HookManager` owns CFG call-site patches, nearby dispatch cells, hit routing,
  and explicit unhook state.
- `AsmHook.asm` only captures a generic register context and tail-jumps to the
  original target. New RCX/RDX/R8/R9 paths do not require assembly changes.
- `FrameRouter` is the extension point for presentation objects that are not
  directly `IDXGISwapChain` compatible.
- `OverlayRenderer` owns all D3D11 and ImGui state. `OverlayUi` has no knowledge
  of hooks or transport.
- `UiStateStore` and `OverlayCommandQueue` are bounded/non-blocking on the
  Present side, ready for a dedicated IPC worker.
- `AnswerIpcService` runs on its own worker thread and consumes a lock-free
  SPSC ring buffer in the named section `Local\CoolHelper.Overlay.Answer.v1`
  written by CoolHelperHub's `OverlayIpcSink`. Both sides heartbeat the
  shared header, so the service reattaches automatically when the hub
  restarts. The transport contract constants are duplicated in
  `IPC/AnswerIpc.h` and CoolHelperHub's `include/coolhelper/OverlayIpc.h` and
  must stay byte-identical. The worker only starts once the runtime is a
  verified owner (hooks installed); the Present thread only takes an SRWLock
  to copy the current markdown text, which `OverlayUi` renders onto the single
  existing overlay window with a dark translucent style and a markdown subset
  (headings with accent underlines, bold, inline code, fenced code blocks,
  bullets and ordered lists with nesting, blockquotes, pipe tables, links,
  horizontal rules) plus streaming auto-scroll.
- Presentation model for the translucent overlay: no aged background
  snapshots. `BackdropCompositor` decides on the GPU whether DWM recomposed
  the overlay region since the previous present, by comparing the region
  against the previous final output: pixels equal to that output were not
  recomposed and keep the saved backdrop, pixels that differ adopt the
  freshly composed content as the new backdrop. The chosen backdrop is
  copied onto the back buffer with a plain CopySubresourceRegion and the UI
  is alpha-blended on top by ImGui; every presented frame also queues the
  region for recomposition. This stays stable whether or not DWM's partial
  recomposition lands on a given frame (desktop/wallpaper repaints are
  irregular), windows passing over the overlay leave no ghosting, and the
  glass shows the live desktop.
- Control channel (`IPC/ControlIpc.h`, mirrored in CoolHelperHub's
  `include/coolhelper/OverlayControl.h`): a small hub-to-DLL ring buffer
  (`Local\CoolHelper.Overlay.Control.v1`) carrying low-frequency commands,
  currently `SetOverlayVisible` (hide/show the overlay window). The DLL
  reports its actual visibility and heartbeats back through the shared
  header, so the hub toggles against the real state and a hub restart
  cannot desync it. The hub registers the global Ctrl+Alt+H hotkey and
  sends the toggle; new command types extend the same ring.

## Adding a Windows build or presentation path

1. Add one or more `PatternVariant` values in
   `Profiles/DwmHookProfiles.cpp`.
2. Add a `HookSpec` with a narrow build range and the verified argument source.
3. If the hook argument is not swap-chain compatible, add an adapter in
   `Render/FrameRouter.cpp`.
4. Test that the function pattern is unique, the selected instruction is the
   intended `FF 15 rel32` CFG dispatch call, and its original dispatcher target
   is executable.
5. Record the tested OS build, `dwmcore.dll` file version, physical/virtual GPU,
   and runtime hit count.

Unsupported builds fail closed. Do not add a broad "first FF 15" fallback to a
new version without verifying the argument contract in a debugger.

## Currently verified

- Windows 10 1909 / build 18363, VMware 3D (`vm3dum64*.dll`), with
  `dwmcore.dll 10.0.18362.752`.
- Windows 10 22H2 / build 19045 on physical hardware.

The profile table also targets the 18362/18363 and 19041-19045 build families,
but every cumulative-update variant should be regression-tested before being
declared verified.

## Lifecycle

The usual injector is CoolHelperHub: it loads the DLL via `CreateRemoteThread`
+ `LoadLibraryW` and unloads by first invoking `ShutdownDwmOverlay` remotely,
then `FreeLibrary`, then verifying the module is gone.

Call the exported `ShutdownDwmOverlay` function before `FreeLibrary`. It stops
the answer-IPC worker, restores the patched call-site displacements, drains
active callbacks, destroys ImGui/D3D11 resources, and frees dispatch-cell
arenas. Cleanup is intentionally not performed from `DLL_PROCESS_DETACH` under
the loader lock.

The overlay window is purely a display surface: lifecycle ownership stays
with the external controller (injection/ejection from CoolHelperHub), and
hooks, IPC state, and renderer resources remain alive for the whole runtime.
`ShutdownDwmOverlay` performs the complete runtime teardown, but does not
decrement the Windows loader reference count. The injector must call
`FreeLibrary` after the export returns. If the injector loaded a temporary
second DLL image to invoke the export, it must release that image as well. A
manually mapped image must be unmapped by the manual mapper because
`FreeLibrary` cannot unload it.
