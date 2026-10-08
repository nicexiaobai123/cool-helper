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
- `HookManager` owns CFG/fothk call-site patches, nearby dispatch cells or
  executable relays, hit routing, and explicit unhook state.
- `AsmHook.asm` captures a generic register context. The Win10 bridge
  tail-jumps to the virtual target; the Win11 bridge returns through a nearby
  relay and the original fothk/XFG dispatcher.
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
   Match the supported OS/module build families; cumulative-update revisions
   can share a profile only while its semantic function/call patterns match.
3. If the hook argument is not swap-chain compatible, add an adapter in
   `Render/FrameRouter.cpp`.
4. Test that the function pattern is unique and that the selected instruction
   is either the intended `FF 15 disp32` CFG dispatch call or an `E8 rel32`
   whose decoded target belongs to the module's `fothk` section.
5. Record the tested OS build, `dwmcore.dll` file version, physical/virtual GPU,
   and runtime hit count.

Unsupported builds fail closed. Do not add a broad "first FF 15" fallback to a
new version without verifying the argument contract in a debugger.

## Currently verified

- Windows 10 1909 / build 18363, VMware 3D (`vm3dum64*.dll`), with
  `dwmcore.dll 10.0.18362.752`.
- Windows 10 22H2 / build 19045 on physical hardware.

Windows 11 has one debugger-verified Legacy presentation profile. The earlier
unhit CD3DDevice Present/alternate/MPO RCX profiles for 26100.9168/9278 have
been removed; they are no longer advertised as supported.

| Eligible build family | Presentation contract |
| --- | --- |
| OS builds `26100-26200`, dwmcore file build `26100` (any revision) | `CLegacySwapChain::Present` -> D2D `PresentDWM`, swap chain in **RDX** |

KD confirmed the composition thread passes through `CLegacyRenderTarget`,
`COverlayContext::Present`, and `CLegacySwapChain::Present`. At call RVA
`0x1BCC92` (`Present+0xB2`), RAX resolves to D2D `PresentDWM` and RDX to
`dxgi!CDXGISwapChainDWMLegacy`. The E8 calls the original guard thunk at RVA
`0x308010`; its loader-retargeted E9 is preserved. RVAs are diagnostic only:
installation requires the supported OS/module build family, a unique
function/call pattern (including RDX setup and vtable +68h), and a validated
fothk dispatch chain. Minor revision, ImageSize, /Brepro stamp and CheckSum
are not pinned; PE metadata is still logged for diagnosis. Missing file
version or out-of-family builds fail closed. This is **not** general Win11
support and is not gated on VMware driver names. Pattern compatibility does
not prove every cumulative update renders correctly: actual overlay
drawing/ghosting validation on the target machine is still required.

The E8 relay saves the register context, routes RDX to the renderer, and
returns to the original guard thunk; it does not replace the global CFG
dispatcher. The first matched callback logs `Hook first hit: ... argument=...`
independently of whether the renderer later obtains a back buffer.

Isolated hook regression tests (no DWM injection):

```powershell
cmake -S tests -B ../analyze_result/win11_legacy_hook_tests_build -G "Visual Studio 16 2019" -A x64
cmake --build ../analyze_result/win11_legacy_hook_tests_build --config Release
ctest --test-dir ../analyze_result/win11_legacy_hook_tests_build -C Release --output-on-failure
```

The CFG-enabled tests map a **synthetic fixture DLL** made from the supplied
KD instruction bytes, not a system dwmcore DLL. They accept varied minor
revisions/PE metadata when patterns match, but reject unsupported OS/module
families, ambiguous functions, wrong call contracts and malformed thunks;
install exactly one Legacy/RDX hook; check RX relay protection, all nine
register/stack arguments, original dispatcher forwarding and return value;
then verify byte-exact restore. They do not prove target-machine rendering.

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
