# CoolHelperHub

Windows x64 C++ middle-tier application for capturing the primary display,
sending a PNG data URL to an OpenAI-compatible vision model, and displaying
the streamed answer in a lightweight Dear ImGui interface.

## Build

For a fast incremental Release build, run:

```powershell
.\build.cmd
```

Common options:

```powershell
.\build.cmd -Configuration Debug     # Debug build
.\build.cmd -Clean                   # Clean rebuild
.\build.cmd -Test                    # Build and run all tests
.\build.cmd -Package                 # Timestamped package under out\
.\build.cmd -Run                     # Package and launch an isolated copy
.\build.cmd -Help                    # Show all options
```

`-Run` launches a timestamped copy so the running process does not lock the
normal build output. The equivalent raw CMake commands are:

```powershell
cmake --preset vs2022-x64
cmake --build --preset release
ctest --preset release
```

The executable is written to `build/Release/CoolHelperHub.exe`.

## First run

Open the Settings tab and configure:

- API Base URL, for example `https://api.openai.com/v1`
- API Key
- Model with vision support
- Optional system and screenshot prompts

The built-in prompt preset is tuned for C/C++, Windows client development,
and reverse-engineering interviews. Code answers target C++17 while remaining
compatible with C++11 where practical; C++20 is used only when a question asks
for it. The Settings tab can restore this preset without overwriting a custom
prompt automatically.

The Settings tab also provides a concise algorithm-interview preset for
LeetCode-style questions. It requests the approach, key insight, complete
C++17/C++11-compatible code, complexity, edge cases, and likely follow-ups.

The default global capture hotkey is `Ctrl+Alt+S`. It can be changed in the
Settings tab using Ctrl/Alt/Shift/Win plus A-Z, 0-9, or F1-F12. You can also
click the capture button or select the tray command to capture the primary
display. Closing the main window hides it to the tray.
Use the tray Exit command to terminate the process.

While the window is hidden, capture hotkeys, successful answers, and errors
remain in the background and do not restore the window. To restore it, right
click the tray icon and select Show Window. Starting a second instance also
leaves the existing tray instance hidden.

The API key is protected with Windows DPAPI before it is written to
`%LOCALAPPDATA%\CoolHelperHub\settings.json`.

## DWM overlay integration

The `DWM 覆盖层` tab manages DwmCfgOverlayLab.dll inside `dwm.exe`:

- **注入 DLL** copies the DLL path into the DWM process and starts it via
  `CreateRemoteThread` + `LoadLibraryW`. The default path is
  `DwmCfgOverlayLab.dll` next to `CoolHelperHub.exe` and can be overridden in
  the tab (persisted in settings).
- **卸载 DLL** first invokes the remote `ShutdownDwmOverlay` export so hooks
  are restored and the runtime drains, then calls remote `FreeLibrary`, and
  verifies the module is gone. If hook restoration fails the DLL stays loaded
  on purpose; the tab reports why.
- Status lines show the DWM process id (active console session), whether the
  DLL is resident, and the IPC connection state, refreshed once per second.

Answers stream to both the local window and the overlay: every `AnswerEvent`
is forwarded by `TeeAnswerSink` to `OverlayIpcSink`, which writes the ordered
UTF-8 markdown stream into a lock-free SPSC ring buffer in a named shared
memory section (`Local\CoolHelper.Overlay.Answer.v1`) and signals a named
event. A dedicated sender thread owns every wait; `Publish` never blocks.
Heartbeats on both sides detect restarts, and the DLL reconnects
automatically. Events are filtered to the active request id so a cancelled
request cannot pollute the overlay text.

Before injecting, the hub verifies the DLL's PE imports resolve on the local
machine and probes the path while impersonating the DWM account token:
`LoadLibraryW` inside `dwm.exe` resolves files as the per-session `DWM-x`
virtual account, which cannot read user profiles, mapped drives, or VM shared
folders. Keep the DLL next to `CoolHelperHub.exe` on a local disk.

The executable manifest requires administrator (`resources/admin-uac.props`
via `VS_PROJECT_IMPORT`) and `SeDebugPrivilege` is enabled at startup, which
is required to open `dwm.exe` with full access.

## Architecture

```text
Win32/ImGui App
  -> GDI primary-display capture -> WIC PNG -> Base64
  -> OpenAIClient (WinHTTP) -> SseParser
  -> TeeAnswerSink -> QueuedAnswerSink -> queued local UI events
                    -> OverlayIpcSink  -> shared memory ring -> DWM overlay
DllInjector: FindDwmProcess / Inject / Eject (CreateRemoteThread)
```

- Network work runs outside the UI thread and can be cancelled.
- Closing the window hides it; it does not stop an active request.
- Starting a new capture cancels the previous request.
- Logs are written to `%LOCALAPPDATA%\CoolHelperHub\logs` without API keys,
  screenshots, Base64 data, prompts, or answer content.
- The Release executable uses the static MSVC runtime. Its remaining DLL
  dependencies are Windows system components.
