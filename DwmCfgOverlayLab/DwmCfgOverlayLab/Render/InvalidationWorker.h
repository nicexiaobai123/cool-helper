#pragma once

#include <Windows.h>
#include <array>

namespace dwm_overlay {

// Normal frame invalidation must not cancel an earlier erase request with
// RDW_NOERASE. Desktop repaint specifically requires RDW_ERASE.
inline UINT InvalidationFlags(bool eraseBackground, bool desktop) noexcept {
    return RDW_INVALIDATE | (desktop ? 0 : RDW_FRAME) |
        ((!desktop || eraseBackground) ? RDW_ALLCHILDREN : 0) |
        (eraseBackground ? RDW_ERASE : 0);
}

// Pure bounded bookkeeping, separately testable without desktop invalidation.
// The owning worker serializes these operations with its SRWLOCK.
class DirtyRegions final {
public:
    void Track(UINT64 key, bool hasCurrent, const RECT& current) noexcept {
        Region* selected = nullptr;
        for (auto& region : regions_) if (region.used && region.key == key) { selected = &region; break; }
        if (!selected && hasCurrent)
            for (auto& region : regions_) if (!region.used) { selected = &region; break; }
        if (!selected) return;
        if (selected->used) Queue(selected->rect, !hasCurrent);
        if (hasCurrent) Queue(current);
        *selected = { key, current, hasCurrent };
    }
    void EraseAll() noexcept {
        for (auto& region : regions_) {
            if (region.used) Queue(region.rect, true);
            region = {};
        }
    }
    void CleanupRect(const RECT& dirty) noexcept { Queue(dirty, true); }
    bool Take(RECT& dirty, bool& eraseBackground) noexcept {
        if (!pending_) return false;
        dirty = dirty_; eraseBackground = eraseBackground_;
        pending_ = eraseBackground_ = false; return true;
    }
private:
    void Queue(const RECT& dirty, bool eraseBackground = false) noexcept {
        if (IsRectEmpty(&dirty)) return;
        if (pending_) UnionRect(&dirty_, &dirty_, &dirty);
        else { dirty_ = dirty; pending_ = true; }
        eraseBackground_ = eraseBackground_ || eraseBackground;
    }
    struct Region { UINT64 key = 0; RECT rect = {}; bool used = false; };
    std::array<Region, 16> regions_;
    RECT dirty_ = {};
    bool pending_ = false;
    bool eraseBackground_ = false;
};

class InvalidationWorker final {
public:
	bool Start() noexcept;
	void Stop(DWORD timeoutMs = 2000) noexcept;
    void QueueTarget(UINT64 key, bool hasCurrentRect, const RECT& currentScreenRect) noexcept;
    void EraseAll() noexcept;
    void QueueCleanupRect(const RECT& screenRect) noexcept;

private:
    struct Invalidation { RECT screenRect; bool eraseBackground; };
	static DWORD WINAPI ThreadEntry(void* parameter) noexcept;
	static BOOL CALLBACK InvalidateIntersectingWindow(
		HWND window,
		LPARAM parameter) noexcept;
	DWORD Run() noexcept;


	SRWLOCK lock_ = SRWLOCK_INIT;
	HANDLE event_ = nullptr;
	HANDLE thread_ = nullptr;
	volatile LONG stopping_ = 0;
    DirtyRegions regions_;
};

} // namespace dwm_overlay
