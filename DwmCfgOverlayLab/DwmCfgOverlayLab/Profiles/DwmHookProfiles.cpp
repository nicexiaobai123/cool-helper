#include "DwmHookProfiles.h"
#include "../Common/Log.h"
#include "../Scanner/PatternScanner.h"

#include <cstring>

namespace dwm_overlay {

static constexpr BytePattern kCfgCallPattern = { "\xFF\x15", "xx" };

static const PatternVariant kModernDevicePresentVariants[] = {
	{
		{
			"\x4C\x8B\xDC\x49\x89\x5B\x08\x49\x89\x6B\x10\x49\x89\x73\x18"
			"\x57\x48\x83\xEC\x60\x8B\x99\x68\x04\x00\x00\x41\x8B\xE9"
			"\x48\x8B\xF2\x48\x8B\xF9\x85\xDB",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
		},
		0, 0x90, 0, true, "19041-19045 CD3DDevice::Present"
	}
};

static const PatternVariant kModernPresent1Variants[] = {
	{
		{
			"\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xEC\x30"
			"\x8B\x99\x68\x04\x00\x00\x45\x8B\xD9\x41\x8B\xF0\x4C\x8B\xD2"
			"\x48\x8B\xF9\x85\xDB",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
		},
		0, 0x70, 0, true, "19041-19045 CCompSwapChain::Present1"
	}
};

static const PatternVariant kModernPresentMpoVariants[] = {
	{
		{
			"\x4C\x8B\xDC\x49\x89\x5B\x08\x49\x89\x6B\x10\x49\x89\x73\x18"
			"\x57\x48\x83\xEC\x50\x8B\x99\x68\x04\x00\x00\x41\x8B\xE9"
			"\x48\x8B\xF2\x48\x8B\xF9\x85\xDB",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
		},
		0, 0x90, 0, true, "19041-19045 CD3DDevice::PresentMPO"
	}
};

static const PatternVariant kLegacyD2DPresentVariants[] = {
	{
		{
			"\x48\x89\x5C\x24\x08\x57\x48\x83\xEC\x60\x41\x8A\xC1\x45"
			"\x8B\xD9\xF6\xD0\x41\x8B\xF8\x48\x8B\xDA\xA8\x01\x74\x00",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxx?"
		},
		0, 0x90, 0, true, "18362/18363 D2DPresentDWM"
	}
};

static const PatternVariant kLegacyD2DPresentMpoVariants[] = {
	{
		{
			"\x40\x53\x48\x83\xEC\x50\x41\x8A\xC1\x45\x8B\xD1"
			"\xF6\xD0\x41\x8B\xD8\x4C\x8B\xDA\xA8\x01\x74\x00",
			"xxxxxxxxxxxxxxxxxxxxxxx?"
		},
		0, 0x70, 0, true, "18362/18363 D2DPresentMultiplaneOverlay"
	}
};

static const PatternVariant kLegacyVmwareVariants[] = {
	{
		{
			"\x48\x8B\x89\xF0\x00\x00\x00\x89\x54\x24\x48\x49\x8B\xD7"
			"\x4C\x89\x6C\x24\x40\x44\x89\x6C\x24\x38\x48\x8B\x01",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxx"
		},
		0, 0x50, 0, true, "18362/18363 VMware PresentInternal"
	}
};

static const HookSpec kHookSpecifications[] = {
	{
		HookSiteId::Win10DevicePresent,
		"Win10 CD3DDevice::Present/RDX",
		L"dwmcore.dll", 19041, 19045, 19041, 19045,
		EnvironmentRequirement::Any,
		ArgumentSource::Rdx, PresentKind::DxgiPresent,
		kModernDevicePresentVariants, _countof(kModernDevicePresentVariants)
	},
	{
		HookSiteId::Win10Present1,
		"Win10 CCompSwapChain::Present1/RCX",
		L"dwmcore.dll", 19041, 19045, 19041, 19045,
		EnvironmentRequirement::Any,
		ArgumentSource::Rcx, PresentKind::DxgiPresent1,
		kModernPresent1Variants, _countof(kModernPresent1Variants)
	},
	{
		HookSiteId::Win10PresentMpo,
		"Win10 CD3DDevice::PresentMPO/RDX",
		L"dwmcore.dll", 19041, 19045, 19041, 19045,
		EnvironmentRequirement::Any,
		ArgumentSource::Rdx, PresentKind::MultiplaneOverlay,
		kModernPresentMpoVariants, _countof(kModernPresentMpoVariants)
	},
	{
		HookSiteId::Win10LegacyD2DPresent,
		"Win10 1903/1909 D2DPresentDWM/RDX",
		L"dwmcore.dll", 18362, 18363, 18362, 18363,
		EnvironmentRequirement::Any,
		ArgumentSource::Rdx, PresentKind::DxgiPresent,
		kLegacyD2DPresentVariants, _countof(kLegacyD2DPresentVariants)
	},
	{
		HookSiteId::Win10LegacyD2DPresentMpo,
		"Win10 1903/1909 D2DPresentMPO/RDX",
		L"dwmcore.dll", 18362, 18363, 18362, 18363,
		EnvironmentRequirement::Any,
		ArgumentSource::Rdx, PresentKind::MultiplaneOverlay,
		kLegacyD2DPresentMpoVariants, _countof(kLegacyD2DPresentMpoVariants)
	},
	{
		HookSiteId::Win10VmwarePresentInternal,
		"Win10 1903/1909 VMware PresentInternal/RDX",
		L"dwmcore.dll", 18362, 18363, 18362, 18363,
		EnvironmentRequirement::VmwareD3D,
		ArgumentSource::Rdx, PresentKind::VmwarePresentInternal,
		kLegacyVmwareVariants, _countof(kLegacyVmwareVariants)
	}
};

static bool EnvironmentMatches(
	EnvironmentRequirement requirement,
	const SystemFingerprint& fingerprint) noexcept {
	return requirement == EnvironmentRequirement::Any ||
		(requirement == EnvironmentRequirement::VmwareD3D &&
			fingerprint.vmwareD3D);
}

static UINT64 ResolveHookCallSite(const HookSpec& specification) noexcept {
	const HMODULE module = GetModuleHandleW(specification.moduleName);
	if (!module) {
		DWM_LOG_FORMAT("%s: module is not loaded", specification.name);
		return 0;
	}
	ModuleCodeView code = {};
	if (!TryGetModuleCodeView(module, code)) {
		DWM_LOG_FORMAT("%s: module code section is invalid", specification.name);
		return 0;
	}

	for (SIZE_T index = 0; index < specification.variantCount; ++index) {
		const auto& variant = specification.variants[index];
		const UINT64 functionAddress = FindUniquePatternInCode(
			module, variant.functionPattern, variant.variantName);
		if (!functionAddress)
			continue;
		const UINT64 codeStart = reinterpret_cast<UINT64>(code.codeBase);
		const UINT64 codeEnd = codeStart + code.codeSize;
		const UINT64 searchStart = functionAddress + variant.callSearchOffset;
		if (searchStart < functionAddress || searchStart < codeStart ||
			searchStart > codeEnd ||
			variant.callSearchLength > codeEnd - searchStart) {
			DWM_LOG_FORMAT("%s: call search range is outside .text",
				variant.variantName);
			continue;
		}

		try {
			const auto calls = FindPatternMatchesInRange(
				searchStart,
				variant.callSearchLength, kCfgCallPattern,
				variant.requireUniqueCall ? 2 : variant.callOrdinal + 1);
			if (variant.requireUniqueCall && calls.size() != 1) {
				DWM_LOG_FORMAT("%s: CFG call is missing or ambiguous",
					variant.variantName);
				continue;
			}
			if (calls.size() <= variant.callOrdinal) {
				DWM_LOG_FORMAT("%s: CFG call ordinal %zu not found",
					variant.variantName, variant.callOrdinal);
				continue;
			}
			DWM_LOG_FORMAT("Resolved %s with variant %s",
				specification.name, variant.variantName);
			return calls[variant.callOrdinal];
		}
		catch (...) {
			DWM_LOG_FORMAT("%s: call-site scan failed", variant.variantName);
		}
	}
	return 0;
}

ProfileInstallResult InstallCompatibleHookProfiles(
	const SystemFingerprint& fingerprint,
	HookManager& hookManager) noexcept {
	ProfileInstallResult result = {};
	for (const auto& specification : kHookSpecifications) {
		if (fingerprint.osBuild < specification.minimumBuild ||
			fingerprint.osBuild > specification.maximumBuild ||
			fingerprint.dwmcoreVersion.build < specification.minimumModuleBuild ||
			fingerprint.dwmcoreVersion.build > specification.maximumModuleBuild ||
			!EnvironmentMatches(specification.environment, fingerprint))
			continue;
		++result.applicableHooks;
		if (hookManager.IsInstalled(specification.id)) {
			++result.installedHooks;
			continue;
		}

		const UINT64 callSite = ResolveHookCallSite(specification);
		if (!callSite)
			continue;
		++result.resolvedHooks;
		if (hookManager.Install(specification, callSite))
			++result.installedHooks;
	}
	if (!result.applicableHooks)
		DWM_LOG_FORMAT("No verified hook profile for Windows build %lu",
			fingerprint.osBuild);
	return result;
}

} // namespace dwm_overlay
