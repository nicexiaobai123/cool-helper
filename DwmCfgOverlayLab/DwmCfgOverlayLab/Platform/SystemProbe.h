#pragma once

#include <Windows.h>

namespace dwm_overlay {

struct ModuleVersion {
	WORD major = 0;
	WORD minor = 0;
	WORD build = 0;
	WORD revision = 0;
};

struct SystemFingerprint {
	DWORD osBuild = 0;
	ModuleVersion dwmcoreVersion = {};
	DWORD dwmcoreImageSize = 0;
	bool vmwareD3D = false;
};

DWORD GetWindowsBuildNumber() noexcept;
bool IsVmwareDwm() noexcept;
SystemFingerprint ProbeSystem() noexcept;

} // namespace dwm_overlay
