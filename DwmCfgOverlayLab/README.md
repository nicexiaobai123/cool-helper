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
  -> Render/FrameRouter -> DxgiSurfaceAdapter / DDisplaySurfaceAdapter
                       -> FrameTarget -> OverlayRenderer (display policy)
                                      -> RenderSession (per display/device)
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
- `FrameRouter` selects a surface adapter. Adapters normalize textures,
  display identity and optional pre/post-draw operations into `FrameTarget`;
  only the DDisplay adapter knows its private ABI. Borrowed callback arguments
  and native objects never escape the synchronous presentation callback.
- `DisplayTopology` resolves display identities; `OverlayRenderer` applies the
  common display policy and manages bounded sessions. `RenderSession` owns
  D3D11, ImGui, fonts and backdrop state for one display/device. `OverlayUi`
  has no knowledge of hooks, Windows versions or transport.
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
  to copy the current markdown text, which `OverlayUi` renders independently
  on each selected display with a dark translucent style and a markdown subset
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
- Control channel (shared `../Shared/OverlayControlProtocol.h`): a small hub-to-DLL ring buffer
  (`Local\CoolHelper.Overlay.Control.v1`) carrying low-frequency commands,
  carrying visibility and scrolling commands. The DLL
  reports its actual visibility and heartbeats back through the shared
  header, so the hub toggles against the real state and a hub restart
  cannot desync it. The hub registers the global Ctrl+Alt+H hotkey and
  sends the toggle; new command types extend the same ring. Reserved header
  fields now carry durable desired display mode and a capability/acknowledgment
  word without changing the v1 size or existing commands. A reconnect reapplies
  the selected mode; an old DLL is reported as not supporting display selection.

### Common display policy and state

The hub offers Compatible, PrimaryOnly and AllDisplays. Bootstrap supplies
the compatibility default (Win10 unrestricted; Win11 primary-only), rather
than embedding this decision in an adapter. Explicit primary/all modes require
a known monitor and an unrotated, correctly sized target. An unmapped legacy
Win10 swap chain may render only under Compatible mode, preserving its previous
behavior; it is never assigned to a monitor based on resolution alone.

Up to eight display/device sessions maintain independent ImGui contexts, DPI
fonts, UI layout and background copies. Each has up to eight chain/resource
backdrops. Verified DXGI flip buffers also carry a generation anchor so
same-size ResizeBuffers retires stale copies. No original GetBuffer texture
reference is retained between callbacks. Unknown native generations are not
guessed: if their bounded cache fills, that target stops drawing safely.

Answers remain shared; each session consumes the same accumulated scroll
commands independently. Mouse coordinates and invalidation rectangles use the
monitor's desktop origin, including negative coordinates. Hide, policy changes
and shutdown erase tracked regions across all outputs. This separation allows
new presentation adapters without duplicating fonts, UI or display policy.

## Adding a Windows build or presentation path

1. Add one or more `PatternVariant` values in
   `Profiles/DwmHookProfiles.cpp`.
2. Add a `HookSpec` with a narrow build range and the verified argument source.
   Match the supported OS/module build families; cumulative-update revisions
   can share a profile only while its semantic function/call patterns match.
3. If the hook argument is not swap-chain compatible, add a surface adapter
   producing `FrameTarget` and register it in `Render/FrameRouter.cpp`. Resolve
   real output identity and keep private ABI/preparation callbacks in the adapter;
   do not add Windows-specific selection or UI code to the shared renderer.
4. Test that the function pattern is unique and that the selected instruction
   is either the intended `FF 15 disp32` CFG dispatch call, an `E8 rel32`
   into `fothk`, or an explicitly verified local callee with its own adapter.
5. Record the tested OS build, `dwmcore.dll` file version, physical/virtual GPU,
   and runtime hit count.

Unsupported builds fail closed. Do not add a broad "first FF 15" fallback to a
new version without verifying the argument contract in a debugger.

## Currently verified

- Windows 10 1909 / build 18363, VMware 3D (`vm3dum64*.dll`), with
  `dwmcore.dll 10.0.18362.752`.
- Windows 10 22H2 / build 19045 on physical hardware.

Windows 11 retains the debugger-verified inlined Legacy profile:
OS `26100-26200`, dwmcore file build `26100`, unique function/call patterns,
RDX = `dxgi!CDXGISwapChainDWMLegacy`, original fothk dispatch preserved.
This is not general Win11 rendering support. Unhit split-Legacy and other
exploratory Present profiles/probes have been removed.

### Win11 DDisplay rendering

The identified OS 26200 / dwmcore 10.0.26100.9278 image
(SizeOfImage `0x443000`, stamp `0x9A1AF3BA`, checksum `0x004477E9`)
uses one **rendering** hook. Inactive Legacy candidates are not installed
on this image; the separate inlined Legacy compatibility profile remains
available to other matching images. Temporary path-discovery probes are removed.

```text
CDDisplaySwapChain::PresentMPO (RVA 140910)
  -> hook its ExecutePresent call (RVA 1409F9)
  -> R8 plane array / R9D count, select enabled base plane 0
  -> CDDisplaySwapChainBuffer::GetD3D11Resource
  -> QueryInterface(ID3D11Texture2D)
  -> FrameTarget -> common display policy -> per-display RenderSession
  -> original CDDisplaySwapChain::ExecutePresent (RVA 2BD2C4)
```

`Profiles/Win11DDisplayProfile.h` owns image/pattern selection.
`Render/DDisplaySurfaceAdapter` owns the verified native ABI and resource
extraction, not HookManager. Register snapshots are borrowed only for the
synchronous callback. No native buffer is cast to `IDXGISwapChain`.

Native ABI evidence from matching PDBs and instructions:
`CDDisplaySwapChainBuffer` vtable RVA `300E48`, slot `+98h` ->
`GetD3D11Resource` (`1F9E90`, borrowed resource).
The scanout is queried for IID `AAC1AA85-B883-5C29-B7C1-C2EAAEB3DA75`;
its `ddisplay.dll` vtable `43ED8`, slot `+30h` ->
`SetPlaneDirtyRects` (`293A0`). Rectangles use X/Y/Width/Height.
Before drawing, replace dirty rectangles with the full frame (a superset of
DWM's original updates); do not replace them with only the UI rectangle.
The adapter additionally validates ddisplay image size `60000` / stamp
`A9FD047A`, actual object vtables and getter entry bytes. Unknown layouts
fail closed; do not widen version gates without re-verifying the native ABI.

The adapter resolves display identity; the common policy selects target monitors:
the swap-chain interface (`this +18h`, vtable `3084A0`) provides display
adapter LUID / VidPn target ID (verified getters `1F1080` / `2BDBA0`).
These identities map to active display paths and GDI monitors via
[QueryDisplayConfig](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig).
Topology is cached for two seconds, not enumerated on every present; concurrent
refresh callers skip rather than wait. Equal monitor resolutions are not
treated as equal display identities. Compatible mode draws on the primary only;
AllDisplays permits every verified supported output with an independent session.
Unknown mappings, rotated/scaled planes, stereo/MSAA/protected surfaces are
skipped with a one-time reason. Supported formats: BGRA8/RGBA8/RGB10A2/RGBA16F.
Hot-plug/primary-monitor switching and hardware-MPO promotion still need
target-machine validation; this is not arbitrary multi-monitor UI placement.
The existing GPU backdrop comparison/restore and asynchronous desktop
invalidation are reused through the common render session. Backgrounds are
isolated per chain/resource; size/format/origin changes recreate the session,
and failed preservation skips drawing. GPU writes are flushed before the original
ExecutePresent. Full-frame dirty rectangles prioritize correctness over
partial-present bandwidth optimization.

Rebuild the DLL, **unload the old instance**, then inject the new one.
Expected milestones: `Win11 DDisplay rendering path selected`,
`Runtime ready with 1 hook(s)`, `Hook first hit`,
`DDisplay texture adapter ready`, `DDisplay base-plane D3D11 texture acquired`,
`Render session created`, `Present source active`, `ImGui initialized`.
Shutdown reports the hit count and releases the renderer's texture/backdrop
references after restoring hooks and draining callbacks.
Target-machine rendering/ghosting/hide-show/unload validation is still required.

Isolated checks (synthetic fixtures/WARP only; no DWM injection):

```powershell
cmake -S tests -B ../analyze_result/win11_legacy_hook_tests_build -G "Visual Studio 16 2019" -A x64
cmake --build ../analyze_result/win11_legacy_hook_tests_build --config Release
ctest --test-dir ../analyze_result/win11_legacy_hook_tests_build -C Release --output-on-failure
```

Tests cover Legacy regression; the DDisplay render callback and register
contract; exclusive hook selection, image/callee validation, nine-argument
forwarding and exact restore; bounded native plane reads, WARP texture QI
ownership and dirty-rect ABI; same-resolution monitor identity selection;
four-format GPU backdrop restore/readback and pipeline-state restoration;
two independent WARP devices/ImGui contexts, negative desktop origins,
per-output erase tracking and repeated same-size DXGI ResizeBuffers.
Shared renderer/router sources are also compiled
without building the production DLL. Win10 profiles remain unchanged.
Hub tests additionally cover settings migration and the real control-channel
worker's display-mode synchronization/reconnect/old-peer compatibility using
isolated test-only object names. These are not target-machine acceptance tests:
Win10/Win11 primary/all modes, different DPI, hot-plug and ghosting still need
validation on the user's physical and VMware systems.

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
