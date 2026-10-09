#include "DDisplaySurfaceAdapter.h"
#include "../Common/Log.h"

#include <cstring>
#include <climits>
#include <limits>
#include <cwchar>

namespace dwm_overlay {
namespace {

bool ReadBytes(UINT64 address, void* output, SIZE_T size) noexcept {
	if (!address || !IsReadableRange(reinterpret_cast<void*>(address), size))
		return false;
	__try {
		std::memcpy(output, reinterpret_cast<void*>(address), size);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ImageMatches(HMODULE module, DWORD size, DWORD stamp) noexcept {
	ModuleCodeView view = {};
	if (!TryGetModuleCodeView(module, view) || view.imageSize != size)
		return false;
	const auto* bytes = reinterpret_cast<const BYTE*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
	return nt->FileHeader.TimeDateStamp == stamp;
}

HRESULT QueryDirtyScanout(void* scanout, ddisplay::IDisplayScanoutDirty** output) noexcept {
	*output = nullptr;
	__try {
		return static_cast<IUnknown*>(scanout)->QueryInterface(
			__uuidof(ddisplay::IDisplayScanoutDirty), reinterpret_cast<void**>(output));
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
}

bool SupportedTexture(const D3D11_TEXTURE2D_DESC& desc) noexcept {
	// Display identity/size are checked separately against active topology.
	const bool format = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
		desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
		desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
	return format && desc.Width && desc.Height &&
		desc.ArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
		desc.SampleDesc.Quality == 0 && desc.Usage == D3D11_USAGE_DEFAULT &&
		(desc.BindFlags & D3D11_BIND_RENDER_TARGET) &&
		!(desc.MiscFlags & D3D11_RESOURCE_MISC_HW_PROTECTED);
}

struct MonitorSearch {
	const WCHAR* device;
	RECT desktop = {};
	bool found = false;
	bool primary = false;
};

BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
	auto& search = *reinterpret_cast<MonitorSearch*>(parameter);
	MONITORINFOEXW info = {};
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) && std::wcscmp(info.szDevice, search.device) == 0) {
		search.desktop = info.rcMonitor;
		search.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
		search.found = true;
		return FALSE;
	}
	return TRUE;
}

} // namespace

namespace ddisplay {

bool IsPrimaryTarget(const DisplayTarget& target, const LUID& adapter,
	UINT32 targetId, UINT width, UINT height) noexcept {
	return target.adapter.LowPart == adapter.LowPart &&
		target.adapter.HighPart == adapter.HighPart && target.targetId == targetId &&
		target.primary && target.unrotated &&
		// Existing UI input and invalidation use primary-desktop coordinates.
		target.desktop.left == 0 && target.desktop.top == 0 &&
		target.desktop.right > 0 && target.desktop.bottom > 0 &&
		width == static_cast<UINT>(target.desktop.right) &&
		height == static_cast<UINT>(target.desktop.bottom);
}

bool SelectBasePlane(UINT64 planes, UINT32 count, PlaneHeader& result) noexcept {
	result = {};
	if (!planes || !count || count > kMaximumPlanes)
		return false;
	bool found = false;
	for (UINT32 index = 0; index < count; ++index) {
		const UINT64 offset = static_cast<UINT64>(index) * 0x80;
		if (planes > (std::numeric_limits<UINT64>::max)() - offset - sizeof(PlaneHeader))
			return false;
		PlaneHeader plane = {};
		if (!ReadBytes(planes + offset, &plane, sizeof(plane)))
			return false;
		if (plane.index != 0 || !plane.enabled || !plane.buffer)
			continue;
		if (found) return false; // Ambiguous base-plane contract: fail closed.
		result = plane;
		found = true;
	}
	return found;
}

HRESULT QueryBufferTexture(void* buffer, GetResource getter, ID3D11Texture2D** texture) noexcept {
	if (!texture) return E_POINTER;
	*texture = nullptr;
	if (!buffer || !getter) return E_INVALIDARG;
	__try {
		ID3D11Resource* resource = getter(buffer);
		return resource ? resource->QueryInterface(__uuidof(ID3D11Texture2D),
			reinterpret_cast<void**>(texture)) : E_NOINTERFACE;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
}

HRESULT SetFullPlaneDirtyRects(IDisplayScanoutDirty* scanout, UINT32 plane,
	UINT width, UINT height) noexcept {
	if (!scanout || !width || !height || width > INT_MAX || height > INT_MAX)
		return E_INVALIDARG;
	RectInt32 full = { 0, 0, static_cast<INT32>(width), static_cast<INT32>(height) };
	__try {
		// Full frame is a superset of DWM's original dirty rectangles. Replacing
		// them with just the UI rect would drop unrelated desktop updates.
		return scanout->SetPlaneDirtyRects(plane, 1, &full);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
}

} // namespace ddisplay

bool DDisplaySurfaceAdapter::EnsureLayout() noexcept {
	AcquireSRWLockExclusive(&layoutLock_);
	const bool valid = EnsureLayoutLocked();
	ReleaseSRWLockExclusive(&layoutLock_);
	return valid;
}

bool DDisplaySurfaceAdapter::EnsureLayoutLocked() noexcept {
	if (layoutChecked_) return layoutValid_;
	const HMODULE core = GetModuleHandleW(L"dwmcore.dll");
	const HMODULE display = GetModuleHandleW(L"ddisplay.dll");
	if (!core || !display) return false;
	layoutChecked_ = true;
	// The profile pins core 26100.9278 (stamp 9A1AF3BA); runtime slot/code
	// checks below independently reject mismatched objects and callees.
	if (!ImageMatches(core, 0x443000, 0x9A1AF3BA) ||
		!ImageMatches(display, 0x60000, 0xA9FD047A)) {
		DWM_LOG_ONCE("DDisplay adapter: unsupported dwmcore/ddisplay image layout");
		return false;
	}
	const UINT64 coreBase = reinterpret_cast<UINT64>(core);
	const UINT64 displayBase = reinterpret_cast<UINT64>(display);
	bufferVtable_ = coreBase + 0x300E48;
	scanoutVtable_ = displayBase + 0x43FC8;
	dirtyVtable_ = displayBase + 0x43ED8;
	chainVtable_ = coreBase + 0x3084A0;
	UINT64 getter = 0, dirtyMethod = 0, luidGetter = 0, targetGetter = 0;
	if (!ReadBytes(bufferVtable_ + 0x98, &getter, sizeof(getter)) ||
		getter != coreBase + 0x1F9E90 ||
		!ValidateLocalCallTarget(core, getter,
			{ "\x48\x8B\x89\xD8\x00\x00\x00\x48\x8B\x01\x48\x8B\x40\x78", "xxxxxxxxxxxxxx" }) ||
		!ReadBytes(dirtyVtable_ + 0x30, &dirtyMethod, sizeof(dirtyMethod)) ||
		dirtyMethod != displayBase + 0x293A0 ||
		!ValidateLocalCallTarget(display, dirtyMethod,
			{ "\x48\x89\x5C\x24\x10\x48\x89\x74\x24\x18\x48\x89\x7C\x24\x20"
			  "\x41\x54\x41\x56\x41\x57\x48\x83\xEC\x50", "xxxxxxxxxxxxxxxxxxxxxxxxx" }) ||
		!ReadBytes(chainVtable_ + 0xD8, &luidGetter, sizeof(luidGetter)) ||
		luidGetter != coreBase + 0x1F1080 ||
		!ValidateLocalCallTarget(core, luidGetter,
			{ "\x48\x8B\x41\x34\x48\x89\x02\x48\x8B\xC2\xC3", "xxxxxxxxxxx" }) ||
		!ReadBytes(chainVtable_ + 0xE8, &targetGetter, sizeof(targetGetter)) ||
		targetGetter != coreBase + 0x2BDBA0 ||
		!ValidateLocalCallTarget(core, targetGetter, { "\x8B\x41\x3C\xC3\xCC", "xxxxx" })) {
		DWM_LOG_ONCE("DDisplay adapter: native resource/dirty-rect method validation failed");
		return false;
	}
	getResource_ = reinterpret_cast<ddisplay::GetResource>(getter);
	layoutValid_ = true;
	DWM_LOG("DDisplay texture adapter ready: GetD3D11Resource + scanout dirty-rect interface verified");
	return true;
}

void DDisplaySurfaceAdapter::RefreshDisplays() noexcept {
	// Bounded, cached OS queries, not per-present enumeration. No resolution-
	// based fallback: stale/missing display identity must fail closed.
	displayCount_ = 0;
	DISPLAYCONFIG_PATH_INFO paths[32] = {};
	DISPLAYCONFIG_MODE_INFO modes[64] = {};
	UINT32 pathCount = 32, modeCount = 64;
	const LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount,
		paths, &modeCount, modes, nullptr);
	if (result != ERROR_SUCCESS) {
		DWM_LOG_ONCE("DDisplay adapter: active display topology unavailable; frame skipped");
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
		if (!search.found) continue;
		auto& target = displays_[displayCount_++];
		target.adapter = path.targetInfo.adapterId;
		target.targetId = path.targetInfo.id;
		target.desktop = search.desktop;
		target.primary = search.primary;
		target.unrotated = path.targetInfo.rotation == DISPLAYCONFIG_ROTATION_IDENTITY;
	}
}

bool DDisplaySurfaceAdapter::IsPrimaryDisplay(UINT64 chain, UINT width, UINT height) noexcept {
	UINT64 vtable = 0;
	LUID adapter = {};
	UINT32 targetId = 0;
	// ExecutePresent RCX is the object base; the verified swap-chain interface
	// lives at +18h. Its display LUID/target getters read +34h/+3Ch respectively.
	if (!chain || chain > (std::numeric_limits<UINT64>::max)() - 0x58 ||
		!ReadBytes(chain + 0x18, &vtable, sizeof(vtable)) || vtable != chainVtable_ ||
		!ReadBytes(chain + 0x4C, &adapter, sizeof(adapter)) ||
		!ReadBytes(chain + 0x54, &targetId, sizeof(targetId))) {
		DWM_LOG_ONCE("DDisplay adapter: unverified swap-chain display identity; frame skipped");
		return false;
	}
	// Concurrent callers skip rather than wait behind a topology refresh.
	if (!TryAcquireSRWLockExclusive(&displayLock_)) return false;
	const ULONGLONG now = GetTickCount64();
	if (now >= nextDisplayRefresh_) {
		RefreshDisplays();
		nextDisplayRefresh_ = GetTickCount64() + 2000;
	}
	bool primary = false;
	for (UINT32 index = 0; index < displayCount_; ++index) {
		if (ddisplay::IsPrimaryTarget(displays_[index], adapter, targetId, width, height)) {
			primary = true;
			break;
		}
	}
	ReleaseSRWLockExclusive(&displayLock_);
	if (primary) {
		DWM_LOG_ONCE("DDisplay primary display identified by adapter LUID/target ID");
	}
	else {
		DWM_LOG_ONCE("DDisplay adapter: secondary/rotated/scaled or unmapped display skipped");
	}
	return primary;
}

bool DDisplaySurfaceAdapter::Acquire(const HookCpuContext& context, DDisplayFrame& frame) noexcept {
	frame = {};
	if (!EnsureLayout()) return false;
	ddisplay::PlaneHeader plane = {};
	if (!ddisplay::SelectBasePlane(context.r8, static_cast<UINT32>(context.r9), plane)) {
		DWM_LOG_ONCE("DDisplay adapter: no unambiguous enabled base plane; frame skipped");
		return false;
	}
	UINT64 vtable = 0, owner = 0, ownerVtable = 0, ownerGetter = 0;
	if (plane.buffer > (std::numeric_limits<UINT64>::max)() - 0xE0 ||
		!ReadBytes(plane.buffer, &vtable, sizeof(vtable)) || vtable != bufferVtable_ ||
		!ReadBytes(plane.buffer + 0xD8, &owner, sizeof(owner)) ||
		!ReadBytes(owner, &ownerVtable, sizeof(ownerVtable)) ||
		ownerVtable > (std::numeric_limits<UINT64>::max)() - 0x80 ||
		!ReadBytes(ownerVtable + 0x78, &ownerGetter, sizeof(ownerGetter)) ||
		!IsExecutableAddress(reinterpret_cast<void*>(ownerGetter))) {
		DWM_LOG_ONCE("DDisplay adapter: unverified CDDisplaySwapChainBuffer/resource owner; frame skipped");
		return false;
	}
	HRESULT result = ddisplay::QueryBufferTexture(reinterpret_cast<void*>(plane.buffer),
		getResource_, frame.texture.GetAddressOf());
	if (FAILED(result) || !frame.texture) {
		DWM_LOG_ONCE("DDisplay adapter: GetD3D11Resource did not expose an ID3D11Texture2D");
		return false;
	}
	D3D11_TEXTURE2D_DESC desc = {};
	frame.texture->GetDesc(&desc);
	if (!SupportedTexture(desc)) {
		DWM_LOG_ONCE("DDisplay adapter: unsupported format/protected texture; frame skipped");
		return false;
	}
	if (!IsPrimaryDisplay(context.rcx, desc.Width, desc.Height)) return false;
	if (!ReadBytes(context.rdx, &vtable, sizeof(vtable)) || vtable != scanoutVtable_ ||
		FAILED(QueryDirtyScanout(reinterpret_cast<void*>(context.rdx), frame.dirtyScanout.GetAddressOf())) ||
		!frame.dirtyScanout ||
		!ReadBytes(reinterpret_cast<UINT64>(frame.dirtyScanout.Get()), &vtable, sizeof(vtable)) ||
		vtable != dirtyVtable_) {
		DWM_LOG_ONCE("DDisplay adapter: scanout dirty-rect interface unavailable; frame skipped");
		return false;
	}
	frame.planeIndex = plane.index;
	DWM_LOG_ONCE("DDisplay base-plane D3D11 texture acquired");
	return true;
}

} // namespace dwm_overlay
