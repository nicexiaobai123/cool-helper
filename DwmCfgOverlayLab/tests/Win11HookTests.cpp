#include "../DwmCfgOverlayLab/Profiles/DwmHookProfiles.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace dwm_overlay;
extern "C" UINT64 InvokeRelay(void* relay, void* target);

static SIZE_T callbackCount = 0;
static void OnHook(const HookInvocation& invocation) noexcept {
    if (invocation.argument == reinterpret_cast<void*>(0x22) &&
        invocation.specification->id == HookSiteId::Win11LegacyD2DPresent &&
        invocation.specification->argumentSource == ArgumentSource::Rdx)
        ++callbackCount;
}

static UINT64 __fastcall OriginalTarget(
    UINT64 a, UINT64 b, UINT64 c, UINT64 d, UINT64 e, UINT64 f,
    UINT64 g, UINT64 h, UINT64 i) {
    return a == 0x11 && b == 0x22 && c == 0x33 && d == 0x44 &&
        e == 0x55 && f == 0x66 && g == 0x77 && h == 0x88 && i == 0x99
        ? 0x12345678ABCDEF01ull : 0;
}

static void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

static void WriteBytes(BYTE* address, const void* data, SIZE_T size) {
    DWORD previous = 0;
    Check(VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous) != 0,
        "Cannot change fixture protection");
    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignored = 0;
    Check(VirtualProtect(address, size, previous, &ignored) != 0,
        "Cannot restore fixture protection");
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: DwmHookTests <synthetic fixture dwmcore.dll path>\n");
        return 2;
    }
    // Map only our synthetic fixture, never a system DLL or a live DWM.
    HMODULE module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) {
        std::fprintf(stderr, "LoadLibraryEx failed: %lu\n", GetLastError());
        return 2;
    }
    HookManager hooks;
    HookManager::SetActiveManager(&hooks);
    hooks.SetDispatchCallback(OnHook);
    try {
        BYTE* function = reinterpret_cast<BYTE*>(GetProcAddress(module, "FixturePresent"));
        BYTE* call = reinterpret_cast<BYTE*>(GetProcAddress(module, "FixtureCall"));
        BYTE* thunk = reinterpret_cast<BYTE*>(GetProcAddress(module, "FixtureThunk"));
        BYTE* duplicate = reinterpret_cast<BYTE*>(GetProcAddress(module, "FixtureDuplicate"));
        Check(function && call && thunk && duplicate, "Not a synthetic fixture DLL");
        Check(GetModuleHandleW(L"dwmcore.dll") == module, "Fixture name collision");
        Check(call == function + 0xB2, "Fixture differs from KD call offset");
        Check(thunk[0] == 0xE9, "Fixture guard thunk must start with E9");
        INT32 thunkDelta = 0;
        std::memcpy(&thunkDelta, thunk + 1, sizeof(thunkDelta));
        BYTE* dispatcher = thunk + 5 + thunkDelta;
        std::printf("fixture guard=%p (%02X), dispatcher=%p (%02X %02X %02X) executable=%d/%d\n",
            thunk, thunk[0], dispatcher, dispatcher[0], dispatcher[1], dispatcher[2],
            IsExecutableAddress(thunk), IsExecutableAddress(dispatcher));

        ModuleCodeView code = {};
        Check(TryGetModuleCodeView(module, code), "Code section missing");
        const std::vector<BYTE> before(code.codeBase, code.codeBase + code.codeSize);
        const auto image = reinterpret_cast<const BYTE*>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        const auto probed = ProbeSystem();
        Check(probed.dwmcoreImageSize == nt->OptionalHeader.SizeOfImage &&
            probed.dwmcoreImageStamp == nt->FileHeader.TimeDateStamp &&
            probed.dwmcoreImageChecksum == nt->OptionalHeader.CheckSum,
            "System probe must read identity from the loaded image");
        SystemFingerprint profile = {};
        profile.osBuild = 26200;
        profile.dwmcoreVersion = { 10, 0, 26100, 9278 };
        profile.dwmcoreImageSize = 0x443000;
        profile.dwmcoreImageStamp = 0x6FDE2E0A;
        profile.dwmcoreImageChecksum = 0x00440845;
        // Bound OS/module families, but do not pin a cumulative-update
        // revision, PE stamp, checksum, image size or VMware driver flag.
        for (unsigned field = 0; field < 5; ++field) {
            auto rejected = profile;
            if (field == 0) rejected.osBuild = 26099;
            if (field == 1) rejected.osBuild = 26201;
            if (field == 2) rejected.dwmcoreVersion.build = 26099;
            if (field == 3) rejected.dwmcoreVersion.build = 26101;
            if (field == 4) rejected.dwmcoreVersion = {};
            const auto result = InstallCompatibleHookProfiles(rejected, hooks);
            Check(result.applicableHooks == 0 && hooks.InstalledCount() == 0,
                "Unsupported OS/module build family must be rejected");
        }
        const WORD revisions[] = { 0, 5670, 9278, 0xFFFF };
        for (unsigned index = 0; index < _countof(revisions); ++index) {
            auto accepted = profile;
            accepted.osBuild = index % 2 ? 26100 : 26200;
            accepted.dwmcoreVersion.revision = revisions[index];
            accepted.dwmcoreImageSize = index == 3 ? 0 : 0x443000 + index * 0x1000;
            accepted.dwmcoreImageStamp = index == 3 ? 0 : 0x9A1AF3BA + index;
            accepted.dwmcoreImageChecksum = index == 3 ? 0 : 0x004477E9 + index;
            HookManager candidate;
            const auto result = InstallCompatibleHookProfiles(accepted, candidate);
            const bool restored = candidate.UninstallAll();
            Check(restored && result.applicableHooks == 1 &&
                result.resolvedHooks == 1 && result.installedHooks == 1,
                "Matching patterns must accept different minor revisions/image metadata");
            Check(std::memcmp(before.data(), code.codeBase, before.size()) == 0,
                "Revision-compatibility test must restore .text");
        }

        const BYTE originalThunkOpcode = thunk[0];
        const BYTE nop = 0x90;
        WriteBytes(thunk, &nop, 1);
        auto result = InstallCompatibleHookProfiles(profile, hooks);
        WriteBytes(thunk, &originalThunkOpcode, 1);
        Check(result.resolvedHooks == 1 && result.installedHooks == 0,
            "Malformed guard thunk must be rejected");

        const std::vector<BYTE> savedDuplicate(duplicate, duplicate + 47);
        WriteBytes(duplicate, function, 47);
        result = InstallCompatibleHookProfiles(profile, hooks);
        WriteBytes(duplicate, savedDuplicate.data(), savedDuplicate.size());
        Check(result.resolvedHooks == 0 && result.installedHooks == 0,
            "Ambiguous function pattern must be rejected");

        // Preserve the opcode but break the virtual +68h argument contract.
        const BYTE originalSlot = function[0x9D];
        Check(originalSlot == 0x68, "Fixture vtable offset differs from KD");
        const BYTE wrongSlot = 0x70;
        WriteBytes(function + 0x9D, &wrongSlot, 1);
        result = InstallCompatibleHookProfiles(profile, hooks);
        WriteBytes(function + 0x9D, &originalSlot, 1);
        Check(result.resolvedHooks == 0 && result.installedHooks == 0,
            "Wrong call/argument pattern must be rejected");

        // Two equally plausible call contracts must not become a first-match
        // fallback. The synthetic function is not executed in this test.
        const std::vector<BYTE> callPattern(function + 0x86, function + 0x86 + 53);
        const std::vector<BYTE> savedSearch(function + 0x60, function + 0xD0);
        std::vector<BYTE> ambiguousCalls(savedSearch.size(), 0xCC);
        std::memcpy(ambiguousCalls.data(), callPattern.data(), callPattern.size());
        std::memcpy(ambiguousCalls.data() + 53, callPattern.data(), callPattern.size());
        WriteBytes(function + 0x60, ambiguousCalls.data(), ambiguousCalls.size());
        result = InstallCompatibleHookProfiles(profile, hooks);
        WriteBytes(function + 0x60, savedSearch.data(), savedSearch.size());
        Check(result.resolvedHooks == 0 && result.installedHooks == 0,
            "Ambiguous call pattern must be rejected");

        // A valid E9 opcode must not make an arbitrary executable destination
        // acceptable as a platform dispatcher.
        INT32 savedThunkDelta = 0;
        std::memcpy(&savedThunkDelta, thunk + 1, sizeof(savedThunkDelta));
        const INT32 badThunkDelta = static_cast<INT32>(function - (thunk + 5));
        WriteBytes(thunk + 1, &badThunkDelta, sizeof(badThunkDelta));
        result = InstallCompatibleHookProfiles(profile, hooks);
        WriteBytes(thunk + 1, &savedThunkDelta, sizeof(savedThunkDelta));
        Check(result.resolvedHooks == 1 && result.installedHooks == 0,
            "Non-dispatcher E9 destination must be rejected");
        Check(std::memcmp(before.data(), code.codeBase, before.size()) == 0,
            "Rejected cases must leave .text unchanged");

        result = InstallCompatibleHookProfiles(profile, hooks);
        Check(result.applicableHooks == 1 && result.resolvedHooks == 1 &&
            result.installedHooks == 1 && hooks.InstalledCount() == 1,
            "Expected exactly one Win11 Legacy/RDX hook");
        INT32 delta = 0;
        std::memcpy(&delta, call + 1, sizeof(delta));
        BYTE* relay = call + 5 + delta;
        MEMORY_BASIC_INFORMATION info = {};
        Check(VirtualQuery(relay, &info, sizeof(info)) != 0 &&
            (info.Protect & 0xFF) == PAGE_EXECUTE_READ, "Relay must be RX");
        UINT64 forwardedTarget = 0;
        std::memcpy(&forwardedTarget, relay + 0x18, sizeof(forwardedTarget));
        Check(forwardedTarget == reinterpret_cast<UINT64>(thunk),
            "Relay must preserve the original dispatcher thunk");
        const UINT64 returned = InvokeRelay(relay, reinterpret_cast<void*>(&OriginalTarget));
        Check(returned == 0x12345678ABCDEF01ull && callbackCount == 1,
            "Relay must route saved RDX and preserve all nine arguments/return");
        for (SIZE_T offset = 0; offset < code.codeSize; ++offset) {
            if (code.codeBase + offset >= call + 1 && code.codeBase + offset < call + 5)
                continue;
            Check(before[offset] == code.codeBase[offset],
                "Hook must change only the selected call displacement");
        }
        Check(hooks.UninstallAll(), "Uninstall failed");
        Check(hooks.UninstallAll(), "Repeated uninstall failed");
        Check(std::memcmp(before.data(), code.codeBase, before.size()) == 0,
            "Uninstall must restore .text byte-for-byte");
        std::puts("PASS: build guards, revision compatibility, unique Legacy call, RDX, 9 arguments, RX relay, exact restore");
        HookManager::SetActiveManager(nullptr);
        FreeLibrary(module);
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
