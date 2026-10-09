#include "DDisplaySurfaceAdapter.h"
#include "../Common/Log.h"

#include <cstring>
#include <climits>
#include <limits>

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

} // namespace

namespace ddisplay {

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

bool DDisplaySurfaceAdapter::ReadDisplayIdentity(UINT64 chain, LUID& adapter, UINT32& targetId) noexcept {
    UINT64 vtable = 0;
    return chain && chain <= (std::numeric_limits<UINT64>::max)() - 0x58 &&
        ReadBytes(chain + 0x18, &vtable, sizeof(vtable)) && vtable == chainVtable_ &&
        ReadBytes(chain + 0x4C, &adapter, sizeof(adapter)) &&
        ReadBytes(chain + 0x54, &targetId, sizeof(targetId));
}

namespace {
HRESULT PrepareDDisplay(void* scanout, UINT32 plane, UINT width, UINT height) noexcept {
    return ddisplay::SetFullPlaneDirtyRects(
        static_cast<ddisplay::IDisplayScanoutDirty*>(scanout), plane, width, height);
}
void FinishDDisplay(ID3D11DeviceContext* context) noexcept { context->Flush(); }
}

bool DDisplaySurfaceAdapter::Acquire(const HookCpuContext& context, DisplayTopology& topology, FrameTarget& frame) noexcept {
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
	LUID adapter = {}; UINT32 targetId = 0;
    if (!ReadDisplayIdentity(context.rcx, adapter, targetId) ||
        !topology.Resolve(adapter, targetId, frame.display) ||
        !FitsDisplay(frame.display, desc.Width, desc.Height)) {
        DWM_LOG_ONCE("DDisplay adapter: unverified/rotated/scaled display target skipped");
        return false;
    }
    Microsoft::WRL::ComPtr<ddisplay::IDisplayScanoutDirty> dirtyScanout;
	if (!ReadBytes(context.rdx, &vtable, sizeof(vtable)) || vtable != scanoutVtable_ ||
		FAILED(QueryDirtyScanout(reinterpret_cast<void*>(context.rdx), dirtyScanout.GetAddressOf())) ||
		!dirtyScanout ||
		!ReadBytes(reinterpret_cast<UINT64>(dirtyScanout.Get()), &vtable, sizeof(vtable)) ||
		vtable != dirtyVtable_) {
		DWM_LOG_ONCE("DDisplay adapter: scanout dirty-rect interface unavailable; frame skipped");
		return false;
	}
	frame.description = desc;
    frame.chainIdentity = context.rcx;
    frame.resourceIdentity = ResourceIdentity(frame.texture.Get());
    if (!frame.resourceIdentity) return false;
    frame.operationOwner = dirtyScanout;
    frame.operationContext = dirtyScanout.Get();
    frame.plane = plane.index;
    frame.PrepareDraw = PrepareDDisplay;
    frame.FinishDraw = FinishDDisplay;
	DWM_LOG_ONCE("DDisplay base-plane D3D11 texture acquired");
	return true;
}

} // namespace dwm_overlay
