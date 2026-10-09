#include "../DwmCfgOverlayLab/Profiles/DwmHookProfiles.h"
#include "../DwmCfgOverlayLab/Profiles/Win11DDisplayProfile.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace dwm_overlay;
extern "C" UINT64 InvokeRelay(void* relay, void* target);
static unsigned callbacks = 0;
static void OnHook(const HookInvocation& invocation) noexcept {
    if (invocation.specification->kind == PresentKind::DDisplayMultiplaneOverlay &&
        invocation.argument == reinterpret_cast<void*>(0x33) && invocation.cpuContext &&
        invocation.cpuContext->rcx == 0x11 && invocation.cpuContext->rdx == 0x22 &&
        invocation.cpuContext->r8 == 0x33 && invocation.cpuContext->r9 == 0x44)
        ++callbacks;
}
static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void WriteBytes(void* address, const void* data, SIZE_T size) {
    DWORD old = 0, ignored = 0;
    Check(VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old) != 0, "Fixture protection");
    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    Check(VirtualProtect(address, size, old, &ignored) != 0, "Fixture protection restore");
}
static UINT64 __fastcall OriginalTarget(UINT64 a, UINT64 b, UINT64 c, UINT64 d,
    UINT64 e, UINT64 f, UINT64 g, UINT64 h, UINT64 i) {
    return a == 0x11 && b == 0x22 && c == 0x33 && d == 0x44 &&
        e == 0x55 && f == 0x66 && g == 0x77 && h == 0x88 && i == 0x99
        ? 0x12345678ABCDEF01ull : 0;
}
static SystemFingerprint Profile() {
    SystemFingerprint profile = {};
    profile.osBuild = 26200;
    profile.dwmcoreVersion = { 10, 0, 26100, 9278 };
    profile.dwmcoreImageSize = 0x443000;
    profile.dwmcoreImageStamp = 0x9A1AF3BA;
    profile.dwmcoreImageChecksum = 0x004477E9;
    return profile;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) return 2;
    HookManager hooks;
    HookManager::SetActiveManager(&hooks);
    hooks.SetDispatchCallback(OnHook);
    try {
        BYTE* function = reinterpret_cast<BYTE*>(GetProcAddress(module, "FixturePresent"));
        BYTE* call = reinterpret_cast<BYTE*>(GetProcAddress(module, "FixtureCall"));
        BYTE* target = reinterpret_cast<BYTE*>(GetProcAddress(module, "FixtureTarget"));
        BYTE* duplicate = reinterpret_cast<BYTE*>(GetProcAddress(module, "FixtureDuplicate"));
        Check(function && call && target && duplicate && GetModuleHandleW(L"dwmcore.dll") == module,
            "Must load only the synthetic fixture as dwmcore");
        Check(call == function + 0xE9, "Evidence call offset");
        ModuleCodeView code = {};
        Check(TryGetModuleCodeView(module, code), "Fixture code");
        const std::vector<BYTE> before(code.codeBase, code.codeBase + code.codeSize);
        auto profile = Profile();
        Check(win11_ddisplay::MatchesImage(profile), "DDisplay image selection");
        for (unsigned field = 0; field < 6; ++field) {
            auto wrong = profile;
            if (field == 0) ++wrong.osBuild;
            if (field == 1) ++wrong.dwmcoreVersion.build;
            if (field == 2) ++wrong.dwmcoreVersion.revision;
            if (field == 3) ++wrong.dwmcoreImageSize;
            if (field == 4) ++wrong.dwmcoreImageStamp;
            if (field == 5) ++wrong.dwmcoreImageChecksum;
            Check(!win11_ddisplay::MatchesImage(wrong), "Adapter must not read unknown internal layouts");
            const auto rejected = InstallCompatibleHookProfiles(wrong, hooks);
            Check(!rejected.installedHooks && !hooks.InstalledCount(), "Wrong image must not install this adapter");
        }
        const BYTE savedTarget = target[0], bad = 0xCC;
        WriteBytes(target, &bad, 1);
        auto result = InstallCompatibleHookProfiles(profile, hooks);
        WriteBytes(target, &savedTarget, 1);
        Check(!result.resolvedHooks && !result.installedHooks, "Wrong callee signature must fail closed");

        WriteBytes(duplicate, function, 24);
        result = InstallCompatibleHookProfiles(profile, hooks);
        BYTE restoredPadding[24]; std::memset(restoredPadding, 0xCC, sizeof(restoredPadding));
        WriteBytes(duplicate, restoredPadding, sizeof(restoredPadding));
        Check(!result.resolvedHooks, "Ambiguous function must fail closed");
        const BYTE savedArgument = function[0xE2]; // mov r8,rsi (4C 8B C6).
        const BYTE wrongArgument = 0xC7;
        WriteBytes(function + 0xE2, &wrongArgument, 1);
        result = InstallCompatibleHookProfiles(profile, hooks);
        WriteBytes(function + 0xE2, &savedArgument, 1);
        Check(!result.resolvedHooks, "Wrong plane argument setup must fail closed");

        auto unsafe = win11_ddisplay::kSpecification;
        unsafe.kind = PresentKind::DxgiPresent;
        Check(!hooks.Install(unsafe, reinterpret_cast<UINT64>(call)), "Local DDisplay call must reject the DXGI argument contract");

        result = InstallCompatibleHookProfiles(profile, hooks);
        Check(result.applicableHooks == 1 && result.resolvedHooks == 1 &&
            result.installedHooks == 1 && hooks.InstalledCount() == 1, "Exactly one DDisplay render hook, no legacy/debug candidates");
        const auto repeated = InstallCompatibleHookProfiles(profile, hooks);
        Check(repeated.installedHooks == 1 && hooks.InstalledCount() == 1, "Repeated installation is idempotent");
        INT32 delta = 0;
        std::memcpy(&delta, call + 1, sizeof(delta));
        BYTE* relay = call + 5 + delta;
        UINT64 forwarded = 0;
        std::memcpy(&forwarded, relay + 0x18, sizeof(forwarded));
        Check(forwarded == reinterpret_cast<UINT64>(target), "Forward original ExecutePresent, not RAX/CFG");
        MEMORY_BASIC_INFORMATION info = {};
        Check(VirtualQuery(relay, &info, sizeof(info)) && (info.Protect & 0xFF) == PAGE_EXECUTE_READ,
            "Relay must be RX");
        // ONLY the synthetic callee is changed into jmp rax for the ABI test.
        BYTE originalPrefix[2]; std::memcpy(originalPrefix, target, 2);
        const BYTE jumpRax[] = { 0xFF, 0xE0 };
        WriteBytes(target, jumpRax, sizeof(jumpRax));
        const UINT64 returned = InvokeRelay(relay, reinterpret_cast<void*>(&OriginalTarget));
        WriteBytes(target, originalPrefix, sizeof(originalPrefix));
        Check(returned == 0x12345678ABCDEF01ull && callbacks == 1 &&
            hooks.HitCount(HookSiteId::Win11DDisplayPresentMpo) == 1,
            "Preserve nine arguments/return and route native plane/scanout context");
        for (SIZE_T offset = 0; offset < code.codeSize; ++offset) {
            if (code.codeBase + offset >= call + 1 && code.codeBase + offset < call + 5) continue;
            Check(before[offset] == code.codeBase[offset], "Only the one call displacement may change");
        }
        Check(hooks.UninstallAll() && hooks.UninstallAll(), "Uninstall/repeated uninstall");
        Check(std::memcmp(before.data(), code.codeBase, before.size()) == 0, "Byte-exact restore");
        HookManager::SetActiveManager(nullptr);
        FreeLibrary(module);
        std::puts("PASS: one DDisplay rendering callback, native register context, ABI and exact restore");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        hooks.UninstallAll();
        HookManager::SetActiveManager(nullptr);
        FreeLibrary(module);
        return 1;
    }
}
