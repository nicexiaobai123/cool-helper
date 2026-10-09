#pragma once

#include <Windows.h>
#include <array>

namespace dwm_overlay {
struct DisplayTarget {
    HMONITOR monitor = nullptr;
    RECT desktop = {};
    bool primary = false;
    bool unrotated = true;
};
bool FitsDisplay(const DisplayTarget& target, UINT width, UINT height) noexcept;
bool DescribeMonitor(HMONITOR monitor, DisplayTarget& result) noexcept;

// Public Windows display mapping, independent of hook versions/private ABIs.
class DisplayTopology final {
public:
    bool Resolve(const LUID& adapter, UINT32 targetId, DisplayTarget& result) noexcept;
private:
    void Refresh() noexcept;
    struct Entry {
        LUID adapter = {};
        UINT32 targetId = 0;
        DisplayTarget display = {};
    };
    SRWLOCK lock_ = SRWLOCK_INIT;
    ULONGLONG nextRefresh_ = 0;
    std::array<Entry, 32> entries_ = {};
    UINT32 count_ = 0;
};
} // namespace dwm_overlay
