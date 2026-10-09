#include "DisplayTopology.h"
#include "../Common/Log.h"
#include <cwchar>

namespace dwm_overlay {
namespace {
struct MonitorSearch { const WCHAR* device; HMONITOR monitor = nullptr; };
BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& search = *reinterpret_cast<MonitorSearch*>(parameter);
    MONITORINFOEXW info = {}; info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && std::wcscmp(info.szDevice, search.device) == 0) {
        search.monitor = monitor; return FALSE;
    }
    return TRUE;
}
}
bool DescribeMonitor(HMONITOR monitor, DisplayTarget& result) noexcept {
    result = {};
    MONITORINFO info = {}; info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    result.monitor = monitor;
    result.desktop = info.rcMonitor;
    result.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    return true;
}
bool FitsDisplay(const DisplayTarget& target, UINT width, UINT height) noexcept {
    return target.monitor && target.unrotated &&
        static_cast<INT64>(target.desktop.right) - target.desktop.left == width &&
        static_cast<INT64>(target.desktop.bottom) - target.desktop.top == height;
}
void DisplayTopology::Refresh() noexcept {
    count_ = 0;
    DISPLAYCONFIG_PATH_INFO paths[32] = {};
    DISPLAYCONFIG_MODE_INFO modes[64] = {};
    UINT32 pathCount = 32, modeCount = 64;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths,
        &modeCount, modes, nullptr) != ERROR_SUCCESS) {
        DWM_LOG_ONCE("Display topology unavailable; unmapped native targets skipped");
        return;
    }
    for (UINT32 index = 0; index < pathCount; ++index) {
        const auto& path = paths[index];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name = {};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.sourceInfo.adapterId;
        name.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) continue;
        MonitorSearch search = { name.viewGdiDeviceName };
        EnumDisplayMonitors(nullptr, nullptr, FindMonitor, reinterpret_cast<LPARAM>(&search));
        DisplayTarget target;
        if (!DescribeMonitor(search.monitor, target)) continue;
        target.unrotated = path.targetInfo.rotation == DISPLAYCONFIG_ROTATION_IDENTITY;
        entries_[count_++] = { path.targetInfo.adapterId, path.targetInfo.id, target };
    }
}
bool DisplayTopology::Resolve(const LUID& adapter, UINT32 targetId, DisplayTarget& result) noexcept {
    result = {};
    if (!TryAcquireSRWLockExclusive(&lock_)) return false;
    if (GetTickCount64() >= nextRefresh_) {
        Refresh();
        nextRefresh_ = GetTickCount64() + 2000;
    }
    bool found = false;
    for (UINT32 index = 0; index < count_; ++index) {
        const auto& entry = entries_[index];
        if (entry.adapter.LowPart == adapter.LowPart && entry.adapter.HighPart == adapter.HighPart &&
            entry.targetId == targetId) {
            result = entry.display; found = true; break;
        }
    }
    ReleaseSRWLockExclusive(&lock_);
    return found;
}
} // namespace dwm_overlay
