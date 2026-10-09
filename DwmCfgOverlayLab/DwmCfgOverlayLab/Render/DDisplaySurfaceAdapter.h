#pragma once

#include <d3d11.h>
#include <inspectable.h>
#include <wrl/client.h>
#include <array>

#include "../Hooks/HookTypes.h"

namespace dwm_overlay {

namespace ddisplay {

// ABI recovered from 140D1B/140D57 and ddisplay PDB, not the public scanout
// vtable. RectInt32 stores X/Y/Width/Height (NOT RECT's right/bottom).
struct RectInt32 { INT32 x, y, width, height; };
MIDL_INTERFACE("aac1aa85-b883-5c29-b7c1-c2eaaeb3da75")
IDisplayScanoutDirty : public IInspectable {
	virtual HRESULT STDMETHODCALLTYPE SetPlaneDirtyRects(
		UINT32 plane, UINT32 count, RectInt32* rectangles) = 0;
};

struct PlaneHeader {
	UINT32 index;
	BYTE enabled;
	BYTE reserved[3];
	UINT64 buffer;
};
static_assert(sizeof(PlaneHeader) == 16, "DDisplay plane prefix mismatch");
constexpr UINT32 kMaximumPlanes = 16;
bool SelectBasePlane(UINT64 planes, UINT32 count, PlaneHeader& result) noexcept;

// GetD3D11Resource returns a borrowed pointer. Only QI's texture reference
// is owned by the caller; do not Release the borrowed resource separately.
using GetResource = ID3D11Resource* (__fastcall*)(void* buffer);
HRESULT QueryBufferTexture(void* buffer, GetResource getter, ID3D11Texture2D** texture) noexcept;
HRESULT SetFullPlaneDirtyRects(IDisplayScanoutDirty* scanout, UINT32 plane,
	UINT width, UINT height) noexcept;

struct DisplayTarget {
	LUID adapter = {};
	UINT32 targetId = 0;
	RECT desktop = {};
	bool primary = false;
	bool unrotated = false;
};
// Match display identity, not resolution: two monitors can have equal sizes.
bool IsPrimaryTarget(const DisplayTarget& target, const LUID& adapter,
	UINT32 targetId, UINT width, UINT height) noexcept;

} // namespace ddisplay

struct DDisplayFrame {
	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	Microsoft::WRL::ComPtr<ddisplay::IDisplayScanoutDirty> dirtyScanout;
	UINT32 planeIndex = 0;
};

class DDisplaySurfaceAdapter final {
public:
	bool Acquire(const HookCpuContext& context, DDisplayFrame& frame) noexcept;
private:
	bool EnsureLayout() noexcept;
	bool EnsureLayoutLocked() noexcept;
	bool IsPrimaryDisplay(UINT64 chain, UINT width, UINT height) noexcept;
	void RefreshDisplays() noexcept;
	SRWLOCK layoutLock_ = SRWLOCK_INIT;
	bool layoutChecked_ = false;
	bool layoutValid_ = false;
	UINT64 bufferVtable_ = 0;
	UINT64 scanoutVtable_ = 0;
	UINT64 dirtyVtable_ = 0;
	UINT64 chainVtable_ = 0;
	ddisplay::GetResource getResource_ = nullptr;
	SRWLOCK displayLock_ = SRWLOCK_INIT;
	ULONGLONG nextDisplayRefresh_ = 0;
	std::array<ddisplay::DisplayTarget, 32> displays_ = {};
	UINT32 displayCount_ = 0;
};

} // namespace dwm_overlay
