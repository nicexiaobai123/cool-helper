#include "../DwmCfgOverlayLab/Render/RenderSession.h"
#include "../DwmCfgOverlayLab/Render/DxgiSurfaceAdapter.h"
#include "../DwmCfgOverlayLab/Render/InvalidationWorker.h"
#include "../DwmCfgOverlayLab/Render/OverlayRenderer.h"
#include "../DwmCfgOverlayLab/IMGUI/imgui.h"
#include <dxgi1_2.h>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <vector>

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
std::vector<UINT32> ReadPixels(ID3D11Device* device, const FrameTarget& frame) {
    auto desc = frame.description; desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Full readback texture");
    device->GetImmediateContext(&context); context->CopyResource(staging.Get(), frame.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Full readback Map");
    std::vector<UINT32> result(static_cast<size_t>(desc.Width) * desc.Height);
    for (UINT row = 0; row < desc.Height; ++row)
        std::memcpy(result.data() + static_cast<size_t>(row) * desc.Width,
            static_cast<BYTE*>(mapped.pData) + row * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0); return result;
}
struct CleanupOperation { int prepared = 0; bool reject = false; };
int cleanupFinished = 0;
HRESULT PrepareCleanup(void* p, UINT32, UINT width, UINT height) noexcept {
    auto& operation = *static_cast<CleanupOperation*>(p);
    ++operation.prepared;
    return operation.reject || !width || !height ? E_FAIL : S_OK;
}
void FinishCleanup(ID3D11DeviceContext* context) noexcept { ++cleanupFinished; context->Flush(); }
void TestHideCleanup() {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "Cleanup WARP device");
    const float blue[] = { 0, 0, 1, 1 }, green[] = { 0, 1, 0, 1 }, red[] = { 1, 0, 0, 1 };
    auto a = Frame(device.Get(), 256, 192, 0, blue);
    auto b = Frame(device.Get(), 256, 192, 0, green);
    b.chainIdentity = a.chainIdentity; // Two rotating buffers on one chain.
    auto cleanA = ReadPixels(device.Get(), a);
    const auto cleanB = ReadPixels(device.Get(), b);
    CleanupOperation operation; cleanupFinished = 0;
    a.operationContext = b.operationContext = &operation;
    a.PrepareDraw = b.PrepareDraw = PrepareCleanup;
    a.FinishDraw = b.FinishDraw = FinishCleanup;
    HookSpec source = {}; source.name = "isolated hidden-buffer cleanup";
    RenderSession session; RECT dirty = {};
    Check(session.Render(a, source, UiSnapshot{}, nullptr, 0, dirty), "Draw cleanup buffer A");
    Check(ReadPixels(device.Get(), a) != cleanA, "Fixture actually contains visible overlay pixels");
    const RECT regionA = dirty;
    Check(session.Render(b, source, UiSnapshot{}, nullptr, 0, dirty), "Draw cleanup buffer B");
    Check(ReadPixels(device.Get(), b) != cleanB, "Second rotation contains visible overlay pixels");
    // Native dirty-rect failure must leave the cleanup pending for a retry.
    operation.reject = true;
    const auto paintedB = ReadPixels(device.Get(), b);
    Check(!session.RestoreWithoutUi(b, dirty) && session.HasOverlayOutput() &&
        ReadPixels(device.Get(), b) == paintedB, "Rejected preparation does not touch the buffer");
    operation.reject = false;
    Check(session.RestoreWithoutUi(b, dirty) && ReadPixels(device.Get(), b) == cleanB &&
        session.HasOverlayOutput(), "One rotation cleans without clearing the other rotation's state");
    // A fresh desktop pixel inside the old UI region must survive restoration.
    auto patch = Frame(device.Get(), 1, 1, 0, red);
    const UINT x = static_cast<UINT>((regionA.left + regionA.right) / 2);
    const UINT y = static_cast<UINT>((regionA.top + regionA.bottom) / 2);
    context->CopySubresourceRegion(a.texture.Get(), 0, x, y, 0, patch.texture.Get(), 0, nullptr);
    cleanA[static_cast<size_t>(y) * a.description.Width + x] = ReadPixels(device.Get(), patch)[0];
    Check(session.RestoreWithoutUi(a, dirty) && ReadPixels(device.Get(), a) == cleanA &&
        !session.HasOverlayOutput(), "Remove stale UI while preserving fresh desktop composition");
    Check(operation.prepared == 5 && cleanupFinished == 4,
        "Cleanup uses the same native dirty-rect preparation and final GPU flush callbacks");
    const auto finishedPixels = ReadPixels(device.Get(), a);
    Check(!session.RestoreWithoutUi(a, dirty) && ReadPixels(device.Get(), a) == finishedPixels,
        "Clean buffers are idle rather than repeatedly restored");
    Check(session.Render(a, source, UiSnapshot{}, nullptr, 0, dirty) && session.HasOverlayOutput(),
        "Show after cleanup draws again without rebuilding the session");
    Check(session.RestoreWithoutUi(a, dirty) && ReadPixels(device.Get(), a) == cleanA,
        "A second hide still restores the fresh background");
}
struct HideDuringDraw { OverlayRenderer* renderer; bool invoked = false; };
HRESULT PrepareAndHide(void* p, UINT32, UINT, UINT) noexcept {
    auto& operation = *static_cast<HideDuringDraw*>(p);
    if (!operation.invoked) { operation.invoked = true; operation.renderer->SetOverlayVisible(false); }
    return S_OK;
}
void TestHiddenRouting() {
    ComPtr<ID3D11Device> device;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "Routing WARP device");
    const float blue[] = { 0, 0, 1, 1 };
    auto a = Frame(device.Get(), 256, 192, 0, blue);
    auto b = Frame(device.Get(), 256, 192, 0, blue);
    b.chainIdentity = a.chainIdentity;
    const auto clean = ReadPixels(device.Get(), a);
    HookSpec source = {}; source.name = "isolated hide routing";
    OverlayRenderer renderer;
    // Deliberately do not Initialize: no desktop invalidation worker starts.
    // All tests below operate on synthetic WARP textures, never the desktop.
    renderer.SetCompatibilityDefault(DisplayMode::AllDisplays);
    renderer.Render(a, source); renderer.Render(b, source);
    Check(ReadPixels(device.Get(), a) != clean, "Routing fixture was drawn");
    renderer.SetOverlayVisible(false);
    Check(renderer.NeedsFrameProcessing(), "Hidden routing remains active for cleanup");
    renderer.Render(a, source);
    Check(ReadPixels(device.Get(), a) == clean && renderer.NeedsFrameProcessing(),
        "Hidden first rotation cleans and still waits for second rotation");
    renderer.Render(b, source);
    Check(ReadPixels(device.Get(), b) == clean && !renderer.NeedsFrameProcessing(),
        "Hidden routing idles only after every painted buffer has been restored");
    renderer.SetOverlayVisible(true);
    HideDuringDraw operation = { &renderer };
    a.operationContext = &operation; a.PrepareDraw = PrepareAndHide;
    renderer.Render(a, source);
    Check(operation.invoked && !renderer.IsOverlayVisible() && !renderer.NeedsFrameProcessing() &&
        ReadPixels(device.Get(), a) == clean, "Hide racing a draw cleans that same frame before Present");
    renderer.Shutdown();
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
    const auto originalA = ReadPixels(deviceA.Get(), a);
    const auto originalB = ReadPixels(deviceB.Get(), b);
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
    Check(first.RestoreWithoutUi(a, firstRect) && ReadPixels(deviceA.Get(), a) == originalA,
        "Hide restores all pixels on first display/device");
    Check(second.RestoreWithoutUi(b, secondRect) && secondRect.left < 0 &&
        ReadPixels(deviceB.Get(), b) == originalB && ReadPixels(deviceA.Get(), a) == originalA,
        "Negative-origin display cleanup is independent of primary display/device");
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
    bool eraseBackground = false;
    regions.Track(1, true, primary);
    regions.Track(2, true, secondary);
    Check(regions.Take(dirty, eraseBackground) && !eraseBackground &&
        dirty.left == -200 && dirty.right == 100 && dirty.bottom == 150,
        "Each output is tracked in desktop coordinates");
    regions.Track(2, false, RECT{});
    Check(regions.Take(dirty, eraseBackground) && eraseBackground && EqualRect(&dirty, &secondary),
        "Disabling secondary requests background erase on only that output");
    regions.EraseAll();
    Check(regions.Take(dirty, eraseBackground) && eraseBackground && EqualRect(&dirty, &primary),
        "Hide/policy switch requests background erase on remaining primary");
    regions.EraseAll();
    Check(!regions.Take(dirty, eraseBackground), "Erased outputs do not retain stale regions");
    regions.CleanupRect(secondary); regions.Track(1, true, primary);
    Check(regions.Take(dirty, eraseBackground) && eraseBackground && dirty.left == -200 && dirty.right == 100,
        "In-flight frame invalidation cannot downgrade a pending background erase");
    Check((InvalidationFlags(true, true) & (RDW_ERASE | RDW_ALLCHILDREN)) == (RDW_ERASE | RDW_ALLCHILDREN) &&
        !(InvalidationFlags(false, true) & RDW_NOERASE) && !(InvalidationFlags(false, false) & RDW_NOERASE),
        "Desktop cleanup erases background; later normal frames do not suppress pending erases");
}
}
int main() {
    try {
        TestSessions(); TestHideCleanup(); TestHiddenRouting(); TestDxgiResize(); TestDirtyRegions();
        std::puts("PASS isolated multi-device UI sessions, hide cleanup/races, fresh pixels and DXGI resize");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
