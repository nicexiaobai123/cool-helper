#include "coolhelper/OverlayControl.h"
#include "../../DwmCfgOverlayLab/DwmCfgOverlayLab/IPC/ControlIpc.h"
#include <iostream>
#include <stdexcept>

using namespace coolhelper;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct State { volatile LONG mode = 0, visible = 1, scroll = 0; };
void Command(void* p, UINT32 type, UINT32 value) noexcept {
    auto& state = *static_cast<State*>(p);
    if (type == overlaycontrol::kControlCommandSetDisplayMode) InterlockedExchange(&state.mode, value);
    else if (type == overlaycontrol::kControlCommandSetOverlayVisible) InterlockedExchange(&state.visible, value);
    else if (type == overlaycontrol::kControlCommandScrollAnswer)
        InterlockedExchangeAdd(&state.scroll, value ? 1 : -1);
}
bool Visible(void* p) noexcept { return InterlockedCompareExchange(&static_cast<State*>(p)->visible, 0, 0) != 0; }
UINT32 Mode(void* p) noexcept { return InterlockedCompareExchange(&static_cast<State*>(p)->mode, 0, 0); }
template<class F> void Wait(F predicate, const char* message) {
    const auto deadline = GetTickCount64() + 4000;
    while (!predicate() && GetTickCount64() < deadline) Sleep(10);
    Check(predicate(), message);
}
}
int main() {
    // Compile-time test namespace prevents touching any live hub/DWM channel.
    State state;
    OverlayControlChannel hub;
    dwm_overlay::ControlIpcService dll({ Command, Visible, Mode, &state });
    try {
        std::string error;
        Check(hub.Start(error), "Start isolated control hub");
        Check(hub.SetDisplayMode(DisplayMode::AllDisplays), "Save desired mode before DLL connects");
        dll.Start();
        Wait([&] { hub.KeepAlive(); return hub.QueryDisplayMode() == 2; }, "Attach applies durable mode/capability");
        Check(hub.SetDisplayMode(DisplayMode::PrimaryOnly), "Switch to primary");
        Wait([&] { hub.KeepAlive(); return hub.QueryDisplayMode() == 1; }, "Primary policy acknowledged");
        Check(!hub.SetDisplayMode(static_cast<DisplayMode>(99)), "Reject invalid mode");
        Check(hub.SendSetOverlayVisible(false) && hub.SendScrollAnswer(-1), "Legacy commands remain usable");
        Wait([&] { hub.KeepAlive(); return hub.QueryOverlayVisible() == 0 &&
            InterlockedCompareExchange(&state.scroll, 0, 0) == -1; }, "Visibility/scroll delivered");
        hub.Stop();
        Check(hub.Start(error) && hub.SetDisplayMode(DisplayMode::AllDisplays), "Restart hub with live DLL");
        Wait([&] { hub.KeepAlive(); return hub.QueryDisplayMode() == 2; }, "Restart resynchronizes configuration");
        dll.Stop();
        Check(!hub.IsDllConnected(), "Stopped DLL clears heartbeat");
        HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, overlaycontrol::kControlSectionName);
        Check(mapping != nullptr, "Open isolated test header");
        auto* header = static_cast<overlaycontrol::ControlSharedHeader*>(
            MapViewOfFile(mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0));
        Check(header != nullptr, "Map isolated test header");
        header->dllHeartbeatTick = GetTickCount64(); header->dllDisplayState = 0; header->dllOverlayVisible = 1;
        Check(hub.QueryDisplayMode() == -1 && hub.QueryOverlayVisible() == 1,
            "Older v1 DLL reports unsupported policy without breaking visibility");
        UnmapViewOfFile(header); CloseHandle(mapping);
        hub.Stop();
        std::cout << "PASS durable display configuration, acknowledgements, restart and v1 compatibility\n";
        return 0;
    } catch (const std::exception& error) {
        dll.Stop(); hub.Stop(); std::cerr << error.what() << '\n'; return 1;
    }
}
