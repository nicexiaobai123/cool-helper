#pragma once

#include "../Hooks/HookManager.h"
#include "../Platform/SystemProbe.h"

namespace dwm_overlay {

struct ProfileInstallResult {
	SIZE_T applicableHooks = 0;
	SIZE_T resolvedHooks = 0;
	SIZE_T installedHooks = 0;
};

ProfileInstallResult InstallCompatibleHookProfiles(
	const SystemFingerprint& fingerprint,
	HookManager& hookManager) noexcept;

} // namespace dwm_overlay
