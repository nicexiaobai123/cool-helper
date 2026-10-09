#include "../DwmCfgOverlayLab/Render/RenderSession.h"
#include "../DwmCfgOverlayLab/Render/DxgiSurfaceAdapter.h"
#include "../DwmCfgOverlayLab/Render/InvalidationWorker.h"
#include "../DwmCfgOverlayLab/IMGUI/imgui.h"
#include <dxgi1_2.h>
#include <cstdio>
#include <stdexcept>
#include <cstring>

using namespace dwm_overlay;
using Microsoft::WRL::ComPtr;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
FrameTarget Frame(ID3D11Device* device, UINT width, UINT height, LONG left, const float* color) {
    FrameTarget result;
    result.description.Width = width; result.description.Height = height;
    result.description.MipLevels = result.description.ArraySize = 1;
    result.description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    result.description.SampleDesc.Count = 1;
    result.description.BindFlags = D3D11_BIND_RENDER_TARGET;
    Check(SUCCEEDED(device->CreateTexture2D(&result.description, nullptr, &result.texture)), "Synthetic frame");
    result.resourceIdentity = ResourceIdentity(result.texture.Get());
    result.chainIdentity = result.resourceIdentity;
    result.display.desktop = { left, 0, left + static_cast<LONG>(width), static_cast<LONG>(height) };
    ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(device->CreateRenderTargetView(result.texture.Get(), nullptr, &target)), "Synthetic RTV");
    device->GetImmediateContext(&context); context->ClearRenderTargetView(target.Get(), color);
    return result;
}
UINT32 ReadCenter(ID3D11Device* device, const FrameTarget& frame) {
    auto desc = frame.description; desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Readback texture");
    device->GetImmediateContext(&context); context->CopyResource(staging.Get(), frame.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Readback Map");
    UINT32 value = 0;
    std::memcpy(&value, static_cast<BYTE*>(mapped.pData) + mapped.RowPitch * (desc.Height / 2) + (desc.Width / 2) * 4, 4);
    context->Unmap(staging.Get(), 0); return value;
}
void TestSessions() {
    ComPtr<ID3D11Device> deviceA, deviceB;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &deviceA, nullptr, nullptr)), "WARP A");
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &deviceB, nullptr, nullptr)), "WARP B");
    const float blue[] = { 0, 0, 1, 1 }, green[] = { 0, 1, 0, 1 };
    auto a = Frame(deviceA.Get(), 256, 192, 0, blue);
    auto b = Frame(deviceB.Get(), 320, 240, -320, green);
    HookSpec source = {}; source.name = "isolated normalized frame";
    UiSnapshot ui = {};
    RenderSession first, second;
    ImGuiContext* sentinel = ImGui::CreateContext();
    RECT firstRect = {}, secondRect = {};
    Check(first.Render(a, source, ui, nullptr, 0, firstRect), "First display/device renders");
    Check(ImGui::GetCurrentContext() == sentinel, "Restore external ImGui context after first initialization");
    const auto colorA = ReadCenter(deviceA.Get(), a);
    Check(second.Render(b, source, ui, nullptr, 0, secondRect), "Second display/device/resolution renders");
    Check(ImGui::GetCurrentContext() == sentinel && secondRect.left < 0 && secondRect.right <= 0,
        "Negative output origin translated into desktop invalidation coordinates");
    Check(first.Render(a, source, ui, nullptr, -1, firstRect), "Return to first device with scroll");
    Check(ReadCenter(deviceA.Get(), a) == colorA, "Second display cannot replace first backdrop or accumulate alpha");
    Check(second.Render(b, source, ui, nullptr, -1, secondRect), "Scroll delivered independently to second UI");
    first.Shutdown(); second.Shutdown();
    Check(ImGui::GetCurrentContext() == sentinel, "Per-display shutdown preserves unrelated ImGui context");
    ImGui::DestroyContext(sentinel);
}
void TestDxgiResize() {
    HWND window = CreateWindowExW(0, L"STATIC", L"isolated overlay resize test", WS_POPUP,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "Hidden fixture window");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "Resize WARP device");
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory> factory;
    Check(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))), "DXGI factory");
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = 256; desc.BufferDesc.Height = 192;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.OutputWindow = window; desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain> chain;
    Check(SUCCEEDED(factory->CreateSwapChain(device.Get(), &desc, &chain)), "Hidden synthetic DXGI swap chain");
    DxgiSurfaceAdapter surface;
    FrameTarget frame;
    Check(surface.Acquire(chain.Get(), frame), "Normalized DXGI acquisition");
    HookSpec source = {}; source.name = "isolated DXGI frame";
    RenderSession session; RECT dirty = {};
    Check(session.Render(frame, source, UiSnapshot{}, nullptr, 0, dirty), "Render synthetic DXGI buffer");
    const UINT64 firstToken = frame.resourceIdentity;
    frame = {}; context->ClearState(); context->Flush();
    Check(SUCCEEDED(chain->ResizeBuffers(2, 320, 240, DXGI_FORMAT_UNKNOWN, 0)),
        "Renderer must not retain GetBuffer references across ResizeBuffers");
    Check(surface.Acquire(chain.Get(), frame) && frame.resourceIdentity != firstToken &&
        frame.description.Width == 320, "Recreated DXGI buffer receives fresh identity");
    // More generations than the backdrop slot count: same-size recreation
    // must retire the old generation rather than eventually saturate the cache.
    for (int i = 0; i < 10; ++i) {
        Check(session.Render(frame, source, UiSnapshot{}, nullptr, 0, dirty),
            "Recreated buffer renders without exhausting backdrop slots");
        const UINT64 previous = frame.resourceIdentity;
        frame = {}; context->ClearState(); context->Flush();
        Check(SUCCEEDED(chain->ResizeBuffers(2, 320, 240, DXGI_FORMAT_UNKNOWN, 0)),
            "Repeated same-size ResizeBuffers releases native buffers");
        Check(surface.Acquire(chain.Get(), frame) && frame.resourceIdentity != previous,
            "Each resized buffer has a new identity");
    }
    frame = {}; session.Shutdown(); chain.Reset(); DestroyWindow(window);
}
void TestDirtyRegions() {
    DirtyRegions regions;
    const RECT primary = { 10, 10, 100, 100 }, secondary = { -200, 20, -100, 150 };
    RECT dirty = {};
    regions.Track(1, true, primary);
    regions.Track(2, true, secondary);
    Check(regions.Take(dirty) && dirty.left == -200 && dirty.right == 100 && dirty.bottom == 150,
        "Each output is tracked in desktop coordinates");
    regions.Track(2, false, RECT{});
    Check(regions.Take(dirty) && EqualRect(&dirty, &secondary), "Disabling secondary erases only that output");
    regions.EraseAll();
    Check(regions.Take(dirty) && EqualRect(&dirty, &primary), "Hide/policy switch still erases remaining primary");
    regions.EraseAll();
    Check(!regions.Take(dirty), "Erased outputs do not retain stale regions");
}
}
int main() {
    try {
        TestSessions(); TestDxgiResize(); TestDirtyRegions();
        std::puts("PASS isolated multi-device UI sessions, coordinates, backdrop and DXGI resize");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
