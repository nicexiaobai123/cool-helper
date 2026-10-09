#include "DxgiSurfaceAdapter.h"
#include "../Common/Log.h"
#include <dxgi1_4.h>

namespace dwm_overlay {
using Microsoft::WRL::ComPtr;
bool DxgiSurfaceAdapter::Acquire(void* presentationObject, FrameTarget& frame) noexcept {
    frame = {};
    if (!presentationObject) return false;
    auto* swapChain = reinterpret_cast<IDXGISwapChain*>(presentationObject);
    ComPtr<IDXGISwapChain> standard;
    auto* owner = swapChain;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&standard)))) owner = standard.Get();
    UINT bufferIndex = 0;
    ComPtr<IDXGISwapChain3> swapChain3;
    if (SUCCEEDED(owner->QueryInterface(IID_PPV_ARGS(&swapChain3))))
        bufferIndex = swapChain3->GetCurrentBackBufferIndex();
    HRESULT result = owner->GetBuffer(bufferIndex, IID_PPV_ARGS(&frame.texture));
    if (FAILED(result) && bufferIndex != 0)
        result = owner->GetBuffer(0, IID_PPV_ARGS(&frame.texture));
    if (FAILED(result) || !frame.texture) return false;
    frame.texture->GetDesc(&frame.description);
    if (!frame.description.Width || !frame.description.Height) return false;
    frame.chainIdentity = reinterpret_cast<UINT64>(owner);
    frame.resourceIdentity = ResourceIdentity(frame.texture.Get());
    if (!frame.resourceIdentity) return false;
    // Only a verified flip-model interface supplies a stable physical buffer
    // index. Legacy DWM GetBuffer(0) must not be guessed to be an anchor.
    if (swapChain3) frame.generationIdentity = frame.resourceIdentity;
    if (swapChain3 && bufferIndex != 0) {
        ComPtr<ID3D11Texture2D> anchor;
        if (FAILED(owner->GetBuffer(0, IID_PPV_ARGS(&anchor)))) return false;
        frame.generationIdentity = ResourceIdentity(anchor.Get());
        if (!frame.generationIdentity) return false;
    }
    // Only invoke standard DXGI output methods on a QI-verified interface.
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc = {};
    if (standard && SUCCEEDED(standard->GetContainingOutput(&output)) &&
        SUCCEEDED(output->GetDesc(&desc))) {
        DescribeMonitor(desc.Monitor, frame.display);
        frame.display.unrotated = desc.Rotation == DXGI_MODE_ROTATION_IDENTITY;
        if (!FitsDisplay(frame.display, frame.description.Width, frame.description.Height))
            frame.display = {}; // compatible mode retains legacy fallback only
    }
    return true;
}
} // namespace dwm_overlay
