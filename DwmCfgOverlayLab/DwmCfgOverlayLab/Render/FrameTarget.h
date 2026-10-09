#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include "DisplayTopology.h"
#include "../../../Shared/OverlayControlProtocol.h"

namespace dwm_overlay {
using DisplayMode = coolhelper_overlay::DisplayMode;

// Owns only this callback's texture reference. Native handles/operation context
// are borrowed and MUST NOT escape the synchronous hook callback.
struct FrameTarget {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC description = {};
    DisplayTarget display = {};
    UINT64 chainIdentity = 0;
    UINT64 resourceIdentity = 0;
    UINT64 generationIdentity = 0; // verified buffer-0 identity when available
    Microsoft::WRL::ComPtr<IUnknown> operationOwner;
    void* operationContext = nullptr;
    UINT32 plane = 0;
    HRESULT (*PrepareDraw)(void*, UINT32, UINT, UINT) noexcept = nullptr;
    void (*FinishDraw)(ID3D11DeviceContext*) noexcept = nullptr;
    UINT64 DisplayKey() const noexcept {
        return display.monitor ? reinterpret_cast<UINT64>(display.monitor)
            : chainIdentity | (UINT64{1} << 63);
    }
};

// Preserve historical Win10 fallback only in Compatible mode. Explicit modes
// require a verified display mapping, never a same-resolution guess.
inline bool AllowsDisplay(const DisplayTarget& target, DisplayMode requested,
    DisplayMode compatibilityDefault) noexcept {
    if (!target.monitor)
        return requested == DisplayMode::Compatible &&
            compatibilityDefault == DisplayMode::AllDisplays;
    const DisplayMode effective = requested == DisplayMode::Compatible
        ? compatibilityDefault : requested;
    return target.unrotated && (effective == DisplayMode::AllDisplays || target.primary);
}
UINT64 ResourceIdentity(ID3D11Texture2D* texture) noexcept;
} // namespace dwm_overlay
