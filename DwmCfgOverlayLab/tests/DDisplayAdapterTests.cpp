#include "../DwmCfgOverlayLab/Render/DDisplaySurfaceAdapter.h"
#include "../DwmCfgOverlayLab/Render/BackdropCompositor.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace dwm_overlay;
using namespace dwm_overlay::ddisplay;
using Microsoft::WRL::ComPtr;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static ID3D11Resource* __fastcall BorrowedResource(void* resource) {
    return static_cast<ID3D11Resource*>(resource);
}
static ID3D11Resource* __fastcall NoResource(void*) { return nullptr; }

class Scanout final : public IDisplayScanoutDirty {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override {
        *object = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPlaneDirtyRects(UINT32 plane, UINT32 count, RectInt32* rectangles) override {
        ++calls; lastPlane = plane; lastCount = count; last = *rectangles; return result;
    }
    unsigned calls = 0;
    UINT32 lastPlane = 0, lastCount = 0;
    RectInt32 last = {};
    HRESULT result = S_OK;
};

static void TestPlanes() {
    BYTE planes[0x100] = {};
    const PlaneHeader input = { 0, 1, {}, 0x12345678 };
    std::memcpy(planes + 0x80, &input, sizeof(input));
    // Disabled first descriptor, enabled base plane in second descriptor.
    PlaneHeader selected = {};
    Check(SelectBasePlane(reinterpret_cast<UINT64>(planes), 2, selected) &&
        selected.buffer == input.buffer && selected.index == 0, "Verified prefix/80h stride");
    std::memcpy(planes, &input, sizeof(input));
    Check(!SelectBasePlane(reinterpret_cast<UINT64>(planes), 2, selected), "Ambiguous base plane rejected");
    Check(!SelectBasePlane(0, 1, selected) && !SelectBasePlane(1, 1, selected) &&
        !SelectBasePlane(reinterpret_cast<UINT64>(planes), kMaximumPlanes + 1, selected) &&
        !SelectBasePlane((std::numeric_limits<UINT64>::max)() - 1, 1, selected),
        "Null, unreadable, too many planes and overflow rejected");

    SYSTEM_INFO info = {}; GetSystemInfo(&info);
    BYTE* region = static_cast<BYTE*>(VirtualAlloc(nullptr, info.dwPageSize * 2,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(region != nullptr, "Snapshot test allocation");
    DWORD old = 0;
    const bool protectedPage = VirtualProtect(region + info.dwPageSize, info.dwPageSize,
        PAGE_NOACCESS, &old) != 0;
    const bool rejected = protectedPage && !SelectBasePlane(
        reinterpret_cast<UINT64>(region + info.dwPageSize - 8), 1, selected);
    VirtualFree(region, 0, MEM_RELEASE);
    Check(rejected, "Cross-page inaccessible snapshot rejected");
}

static void TestResourceOwnership() {
    ComPtr<ID3D11Device> device;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)), "Create isolated WARP device");
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = 16; desc.ArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "Create WARP texture");
    const UINT64 token = ResourceIdentity(texture.Get());
    Check(token && token == ResourceIdentity(texture.Get()), "Stable resource token without retaining buffer");
    ComPtr<ID3D11Texture2D> other;
    Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &other)), "Concurrent identity texture");
    UINT64 concurrent[8] = {}; std::thread workers[8];
    for (int index = 0; index < 8; ++index)
        workers[index] = std::thread([&, index] { concurrent[index] = ResourceIdentity(other.Get()); });
    for (auto& worker : workers) worker.join();
    for (auto value : concurrent)
        Check(value && value == concurrent[0] && value != token, "Concurrent tagging gives one distinct identity");
    const ULONG before = texture->AddRef(); texture->Release();
    ComPtr<ID3D11Texture2D> acquired;
    Check(SUCCEEDED(QueryBufferTexture(texture.Get(), BorrowedResource, &acquired)) &&
        acquired.Get() == texture.Get(), "Borrowed native resource -> owned texture QI");
    acquired.Reset();
    const ULONG after = texture->AddRef(); texture->Release();
    Check(before == after, "No borrowed-resource Release and no leaked QI reference");
    Check(FAILED(QueryBufferTexture(texture.Get(), NoResource, &acquired)) && !acquired,
        "Null resource is not renderable");
    Check(FAILED(QueryBufferTexture(nullptr, BorrowedResource, &acquired)), "Null buffer rejected");
    ComPtr<ID3D11Buffer> buffer;
    D3D11_BUFFER_DESC bufferDesc = {}; bufferDesc.ByteWidth = 16;
    Check(SUCCEEDED(device->CreateBuffer(&bufferDesc, nullptr, &buffer)), "Create non-texture resource");
    Check(FAILED(QueryBufferTexture(buffer.Get(), BorrowedResource, &acquired)) && !acquired,
        "Do not mistake a D3D buffer for a texture");
}

static void TestDirtyRects() {
    Scanout scanout;
    Check(SUCCEEDED(SetFullPlaneDirtyRects(&scanout, 3, 1920, 1080)) &&
        scanout.calls == 1 && scanout.lastPlane == 3 && scanout.lastCount == 1 &&
        scanout.last.x == 0 && scanout.last.y == 0 && scanout.last.width == 1920 &&
        scanout.last.height == 1080, "Correct private-interface ABI and full-frame RectInt32");
    scanout.result = E_FAIL;
    Check(FAILED(SetFullPlaneDirtyRects(&scanout, 0, 640, 480)), "Dirty-rect failure propagated");
    Check(FAILED(SetFullPlaneDirtyRects(&scanout, 0, 0, 480)) &&
        FAILED(SetFullPlaneDirtyRects(nullptr, 0, 640, 480)), "Invalid dirty-rect inputs rejected");
}

static void TestDisplayIdentity() {
    dwm_overlay::DisplayTarget primary;
    primary.monitor = reinterpret_cast<HMONITOR>(1);
    primary.desktop = { 0, 0, 1920, 1080 };
    primary.primary = true;
    dwm_overlay::DisplayTarget secondary = primary;
    secondary.monitor = reinterpret_cast<HMONITOR>(2);
    secondary.primary = false;
    secondary.desktop = { -1920, 0, 0, 1080 };
    Check(AllowsDisplay(primary, DisplayMode::PrimaryOnly, DisplayMode::AllDisplays) &&
        !AllowsDisplay(secondary, DisplayMode::PrimaryOnly, DisplayMode::AllDisplays),
        "Common primary-only policy for both adapters");
    Check(AllowsDisplay(secondary, DisplayMode::AllDisplays, DisplayMode::PrimaryOnly) &&
        FitsDisplay(secondary, 1920, 1080), "Same-size secondary and negative desktop coordinates");
    Check(AllowsDisplay(primary, DisplayMode::Compatible, DisplayMode::AllDisplays) &&
        !AllowsDisplay(secondary, DisplayMode::Compatible, DisplayMode::PrimaryOnly),
        "Compatibility defaults retained outside adapters");
    Check(!FitsDisplay(primary, 1280, 720), "Scaled base plane rejected");
    primary.unrotated = false;
    Check(!AllowsDisplay(primary, DisplayMode::AllDisplays, DisplayMode::AllDisplays),
        "Rotated native plane rejected");
    dwm_overlay::DisplayTarget unknown;
    Check(!AllowsDisplay(unknown, DisplayMode::PrimaryOnly, DisplayMode::AllDisplays) &&
        !AllowsDisplay(unknown, DisplayMode::AllDisplays, DisplayMode::AllDisplays) &&
        AllowsDisplay(unknown, DisplayMode::Compatible, DisplayMode::AllDisplays),
        "Unknown mapping never guessed in explicit policies");
}

static void TestBackdropRoundTrip() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "Backdrop WARP device");
    for (const auto format : { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT }) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = desc.Height = 16; desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1; desc.Format = format;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture, staging;
        ComPtr<ID3D11RenderTargetView> target;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
            SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &target)), "Backdrop texture/RTV");
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Backdrop readback texture");
        const UINT bytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        auto readPixel = [&]() {
            context->CopyResource(staging.Get(), texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Backdrop readback Map");
            UINT64 pixel = 0; std::memcpy(&pixel, mapped.pData, bytes);
            context->Unmap(staging.Get(), 0); return pixel;
        };
        texture->GetDesc(&desc);
        BackdropCompositor compositor;
        Check(compositor.EnsureSize(device.Get(), desc), "Backdrop resources (including HDR pitch)");
        const float background[4] = { 0, 0, 1, 1 }, overlay[4] = { 1, 0, 0, 1 };
        context->ClearRenderTargetView(target.Get(), background);
        const UINT64 clean = readPixel();
        const RECT area = { 0, 0, 16, 16 };
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        const D3D11_VIEWPORT viewport = { 3, 4, 5, 6, 0, 1 };
        context->RSSetViewports(1, &viewport);
        compositor.CaptureRegion(context.Get(), texture.Get(), area);
        Check(compositor.RestoreBackdrop(context.Get(), texture.Get(), area), "Initial backdrop preservation");
        Check(readPixel() == clean, "Initial desktop pixels preserved");
        context->ClearRenderTargetView(target.Get(), overlay);
        compositor.SaveOutput(context.Get(), texture.Get(), area);
        Check(readPixel() != clean, "Simulated overlay output");
        compositor.CaptureRegion(context.Get(), texture.Get(), area);
        Check(compositor.RestoreBackdrop(context.Get(), texture.Get(), area), "Repeated-buffer restoration");
        Check(readPixel() == clean, "Unrecomposed overlay removed without alpha accumulation");
        D3D11_PRIMITIVE_TOPOLOGY topology = {};
        context->IAGetPrimitiveTopology(&topology);
        UINT viewportCount = 1; D3D11_VIEWPORT restoredViewport = {};
        context->RSGetViewports(&viewportCount, &restoredViewport);
        Check(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST && viewportCount == 1 &&
            restoredViewport.TopLeftX == viewport.TopLeftX && restoredViewport.Width == viewport.Width,
            "Backdrop pass restores caller pipeline state");
    }
}

int main() {
    try {
        TestPlanes(); TestResourceOwnership(); TestDirtyRects(); TestDisplayIdentity(); TestBackdropRoundTrip();
        std::puts("PASS: native planes, resource ownership, dirty-rect ABI, multi-monitor identity and GPU backdrop restore");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
