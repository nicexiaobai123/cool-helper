#pragma once

#include <Windows.h>

#include <string>

namespace coolhelper {

// Injection and safe-ejection helpers for DwmCfgOverlayLab.dll into dwm.exe.
// All functions are synchronous and expect the caller to run elevated; the
// process manifest requires administrator anyway.

// Enables SeDebugPrivilege for the current process. Required to open the DWM
// process with full access even when elevated.
bool EnableDebugPrivilege(std::string& error) noexcept;

struct DwmProcessInfo {
	DWORD processId = 0;
	DWORD sessionId = 0;
};

// Finds the dwm.exe serving the active console session.
bool FindDwmProcess(DwmProcessInfo* info, std::string& error) noexcept;

// Returns true when the process currently hosts the DLL. Not finding the DLL
// is a normal outcome (out=false, no error).
bool IsDllLoadedInProcess(
	DWORD processId, const std::wstring& dllPath, bool* loaded,
	std::string& error) noexcept;

// Injects via CreateRemoteThread + LoadLibraryW and verifies the module is
// resident in the target before returning.
bool InjectDllIntoProcess(
	DWORD processId, const std::wstring& dllPath, std::string& error) noexcept;

// Safely unloads: invokes the exported ShutdownDwmOverlay in the remote
// process first (restores hooks, tears the runtime down), then FreeLibrary,
// then verifies the module is gone. If hook restoration fails, the DLL stays
// loaded and the error explains why.
bool EjectDllFromProcess(
	DWORD processId, const std::wstring& dllPath, std::string& error) noexcept;

} // namespace coolhelper
