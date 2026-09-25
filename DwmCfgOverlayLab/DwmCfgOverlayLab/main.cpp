#include <Windows.h>

#include "Bootstrap/Runtime.h"
#include "Common/Log.h"
#include "Platform/SystemProbe.h"

namespace {

HMODULE g_module = nullptr;
volatile LONG g_initializationReady = 0;

DWORD WINAPI InitThreadProc(void*) noexcept {
	for (int index = 0; index < 200; ++index) {
		if (GetModuleHandleW(L"dwmcore.dll"))
			break;
		Sleep(25);
	}

	auto& runtime = dwm_overlay::GetRuntime();
	runtime.SetModule(g_module);
	const bool initialized = runtime.Initialize();
	InterlockedExchange(&g_initializationReady, 1);
	if (!initialized)
		return 0;

	// VMware's UMD can appear after dwmcore. Add the environment-specific
	// profile without disturbing hooks that are already active.
	const DWORD build = dwm_overlay::GetWindowsBuildNumber();
	if (build == 18362 || build == 18363) {
		for (int index = 0; index < 20; ++index) {
			if (runtime.RefreshVmwareProfile()) {
				DWM_LOG("Late VMware hook profile installed");
				break;
			}
			Sleep(250);
		}
	}
	return 0;
}

} // namespace

extern "C" __declspec(dllexport) BOOL WINAPI ShutdownDwmOverlay() noexcept {
	// An injector may load a second image and immediately invoke this export.
	// Wait until that image has registered as owner or control client so the
	// shutdown request cannot race its initialization thread.
	const ULONGLONG deadline = GetTickCount64() + 5000;
	while (InterlockedCompareExchange(&g_initializationReady, 0, 0) == 0 &&
		GetTickCount64() < deadline)
		Sleep(1);
	if (InterlockedCompareExchange(&g_initializationReady, 0, 0) == 0) {
		DWM_LOG("Shutdown requested before instance coordination was ready");
		return FALSE;
	}
	return dwm_overlay::GetRuntime().Shutdown() ? TRUE : FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_module = module;
		InterlockedExchange(&g_initializationReady, 0);
		// The hub's injector loads this DLL inertly to capture the loader's
		// error code when a remote LoadLibraryW fails: no init thread, so the
		// probe can LoadLibrary/FreeLibrary without lifecycle races.
		char probeFlag[8] = {};
		if (GetEnvironmentVariableA("DWM_CFG_OVERLAY_PROBE",
			probeFlag, sizeof(probeFlag)) == 1 && probeFlag[0] == '1') {
			InterlockedExchange(&g_initializationReady, 1);
			DWM_LOG("Inert probe load (DWM_CFG_OVERLAY_PROBE=1)");
			return TRUE;
		}
		DWM_LOG("DLL_PROCESS_ATTACH");
		DisableThreadLibraryCalls(module);
		const HANDLE thread = CreateThread(
			nullptr, 0, InitThreadProc, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
		else {
			InterlockedExchange(&g_initializationReady, 1);
			DWM_LOG("Unable to create initialization thread");
		}
	}
	else if (reason == DLL_PROCESS_DETACH && !reserved) {
		// Restoring code pages or waiting under the loader lock is unsafe. The
		// injector must call ShutdownDwmOverlay before FreeLibrary.
		DWM_LOG("DLL_PROCESS_DETACH; no loader-lock cleanup performed");
	}
	return TRUE;
}
