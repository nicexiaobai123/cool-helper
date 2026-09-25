#include "SystemProbe.h"
#include "../Common/Log.h"

#include <TlHelp32.h>
#include <wchar.h>
#include <vector>
#include <winver.h>

#pragma comment(lib, "version.lib")

namespace dwm_overlay {

DWORD GetWindowsBuildNumber() noexcept {
	using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

	const auto ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll)
		return 0;
	const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
		GetProcAddress(ntdll, "RtlGetVersion"));
	if (!rtlGetVersion)
		return 0;

	OSVERSIONINFOW version = {};
	version.dwOSVersionInfoSize = sizeof(version);
	return rtlGetVersion(&version) >= 0 ? version.dwBuildNumber : 0;
}

static bool IsVmwareD3DModuleName(const wchar_t* moduleName) noexcept {
	static constexpr wchar_t prefix[] = L"vm3dum64";
	const SIZE_T nameLength = wcslen(moduleName);
	const SIZE_T prefixLength = _countof(prefix) - 1;
	return nameLength >= prefixLength + 4 &&
		_wcsnicmp(moduleName, prefix, prefixLength) == 0 &&
		_wcsicmp(moduleName + nameLength - 4, L".dll") == 0;
}

bool IsVmwareDwm() noexcept {
	if (GetModuleHandleW(L"vm3dum64.dll") ||
		GetModuleHandleW(L"vm3dum64_10.dll"))
		return true;

	const HANDLE snapshot = CreateToolhelp32Snapshot(
		TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE)
		return false;

	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	bool found = false;
	if (Module32FirstW(snapshot, &entry)) {
		do {
			if (IsVmwareD3DModuleName(entry.szModule)) {
				found = true;
				break;
			}
		} while (Module32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return found;
}

static ModuleVersion GetModuleVersion(HMODULE module) noexcept {
	ModuleVersion result = {};
	if (!module)
		return result;

	wchar_t path[MAX_PATH] = {};
	if (!GetModuleFileNameW(module, path, _countof(path)))
		return result;
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
	if (!size)
		return result;

	std::vector<BYTE> data(size);
	if (!GetFileVersionInfoW(path, 0, size, data.data()))
		return result;
	VS_FIXEDFILEINFO* info = nullptr;
	UINT infoSize = 0;
	if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info),
		&infoSize) || !info || infoSize < sizeof(*info))
		return result;

	result.major = HIWORD(info->dwFileVersionMS);
	result.minor = LOWORD(info->dwFileVersionMS);
	result.build = HIWORD(info->dwFileVersionLS);
	result.revision = LOWORD(info->dwFileVersionLS);
	return result;
}

static DWORD GetImageSize(HMODULE module) noexcept {
	if (!module)
		return 0;
	const auto base = reinterpret_cast<const BYTE*>(module);
	const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
		return 0;
	const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
		base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	return nt->OptionalHeader.SizeOfImage;
}

SystemFingerprint ProbeSystem() noexcept {
	SystemFingerprint fingerprint = {};
	fingerprint.osBuild = GetWindowsBuildNumber();
	const auto dwmcore = GetModuleHandleW(L"dwmcore.dll");
	fingerprint.dwmcoreVersion = GetModuleVersion(dwmcore);
	fingerprint.dwmcoreImageSize = GetImageSize(dwmcore);
	fingerprint.vmwareD3D = IsVmwareDwm();

	DWM_LOG_FORMAT(
		"System profile: build=%lu dwmcore=%u.%u.%u.%u image=0x%lX vmware=%s",
		fingerprint.osBuild,
		fingerprint.dwmcoreVersion.major,
		fingerprint.dwmcoreVersion.minor,
		fingerprint.dwmcoreVersion.build,
		fingerprint.dwmcoreVersion.revision,
		fingerprint.dwmcoreImageSize,
		fingerprint.vmwareD3D ? "yes" : "no");
	return fingerprint;
}

} // namespace dwm_overlay
