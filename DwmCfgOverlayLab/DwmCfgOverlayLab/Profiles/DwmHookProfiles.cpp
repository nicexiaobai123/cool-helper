#include "DwmHookProfiles.h"
#include "Win11DDisplayProfile.h"
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

// Inlined layout verified in a live KD session: CLegacySwapChain::Present calls
// d2d1!D2DDeviceContextBase<...>::PresentDWM through vtable +68h.
// At this call RDX is dxgi!CDXGISwapChainDWMLegacy, not the wrapper's this.
static const PatternVariant kWin11LegacyD2DPresentVariants[] = {
	{
		{
			"\x48\x89\x5C\x24\x08\x44\x89\x44\x24\x18\x89\x54\x24\x10"
			"\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xEC\x60"
			"\x33\xDB\x4C\x8B\xE9\x44\x8B\xFB\x41\xF6\xC0\x02\x0F\x85"
			"\x00\x00\x00\x00",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????"
		},
		0x60, 0x70, 0, true, "Win11 CLegacySwapChain::Present -> D2D PresentDWM",
		{
			"\x49\x8B\xD4\x48\x8B\x01\x48\x89\x5C\x24\x40\x89\x5C\x24\x38"
			"\x48\x89\x5C\x24\x30\x48\x8B\x40\x68\x89\x7C\x24\x28"
			"\x4C\x89\x7C\x24\x20\x44\x8B\xBC\x24\xB0\x00\x00\x00"
			"\x45\x8B\xCF\xE8\x00\x00\x00\x00\x8B\xF8\x85\xC0",
			"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxx"
		},
		44
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
	},
	{
		HookSiteId::Win11LegacyD2DPresent,
		"Win11 CLegacySwapChain D2D PresentDWM/RDX",
		L"dwmcore.dll", 26100, 26200, 26100, 26100,
		EnvironmentRequirement::Any,
		ArgumentSource::Rdx, PresentKind::DxgiPresent,
		kWin11LegacyD2DPresentVariants, _countof(kWin11LegacyD2DPresentVariants),
		// Live KD: PDB 8CA96642735261D40542AA61D93E813B / age 1,
		// function RVA=1BCBE0, call RVA=1BCC92, guard thunk RVA=308010.
		// As on Win10, accept cumulative-update revisions within the module
		// build family only if both semantic patterns and the guard chain match.
		// Image size, /Brepro stamp and checksum remain diagnostic, not gates.
		CallSiteEncoding::RelativeCallToFothk
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
			const BytePattern& callPattern = variant.callPattern.bytes
				? variant.callPattern : kCfgCallPattern;
			const auto calls = FindPatternMatchesInRange(
				searchStart,
				variant.callSearchLength, callPattern,
				variant.requireUniqueCall ? 2 : variant.callOrdinal + 1);
			if (variant.requireUniqueCall && calls.size() != 1) {
				DWM_LOG_FORMAT("%s: call site is missing or ambiguous",
					variant.variantName);
				continue;
			}
			if (calls.size() <= variant.callOrdinal) {
				DWM_LOG_FORMAT("%s: call ordinal %zu not found",
					variant.variantName, variant.callOrdinal);
				continue;
			}
			const UINT64 callSite = calls[variant.callOrdinal] +
				variant.callOpcodeOffset;
			if (callSite < calls[variant.callOrdinal] || callSite >= codeEnd) {
				DWM_LOG_FORMAT("%s: call opcode offset is invalid",
					variant.variantName);
				continue;
			}
			if (specification.callSiteEncoding ==
				CallSiteEncoding::RelativeCallToFothk) {
				if (*reinterpret_cast<const BYTE*>(callSite) != 0xE8 ||
					!IsReadableRange(reinterpret_cast<const void*>(callSite), 5)) {
					DWM_LOG_FORMAT("%s: expected E8 rel32 call is missing",
						variant.variantName);
					continue;
				}
				const INT32 displacement =
					*reinterpret_cast<const INT32*>(callSite + 1);
				const UINT64 target = callSite + 5 + displacement;
				ModuleSectionView fothk = {};
				if (!TryGetModuleSection(module, "fothk", fothk) ||
					target < reinterpret_cast<UINT64>(fothk.base) ||
					target >= reinterpret_cast<UINT64>(fothk.base) + fothk.size) {
					DWM_LOG_FORMAT("%s: E8 target is not in fothk",
						variant.variantName);
					continue;
				}
			}
			else if (specification.callSiteEncoding ==
				CallSiteEncoding::RelativeCallToVerifiedLocal) {
				if (specification.kind != PresentKind::DDisplayMultiplaneOverlay ||
					!IsReadableRange(reinterpret_cast<const void*>(callSite), 5) ||
					*reinterpret_cast<const BYTE*>(callSite) != 0xE8)
					continue;
				INT32 displacement = 0;
				std::memcpy(&displacement, reinterpret_cast<const void*>(callSite + 1), 4);
				const UINT64 target = static_cast<UINT64>(
					static_cast<INT64>(callSite + 5) + displacement);
				if (!ValidateLocalCallTarget(module, target,
					specification.localTargetPattern)) {
					DWM_LOG_FORMAT("%s: local callee pattern is invalid", variant.variantName);
					continue;
				}
			}
			DWM_LOG_FORMAT(
				"Resolved %s with variant %s (function RVA=0x%llX call RVA=0x%llX)",
				specification.name, variant.variantName,
				static_cast<unsigned long long>(functionAddress -
					reinterpret_cast<UINT64>(code.imageBase)),
				static_cast<unsigned long long>(callSite -
					reinterpret_cast<UINT64>(code.imageBase)));
			return callSite;
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
	const auto install = [&](const HookSpec& specification) noexcept {
		if (fingerprint.osBuild < specification.minimumBuild ||
			fingerprint.osBuild > specification.maximumBuild ||
			fingerprint.dwmcoreVersion.build < specification.minimumModuleBuild ||
			fingerprint.dwmcoreVersion.build > specification.maximumModuleBuild ||
			fingerprint.dwmcoreVersion.revision <
				specification.minimumModuleRevision ||
			fingerprint.dwmcoreVersion.revision >
				specification.maximumModuleRevision ||
			(specification.requiredImageSize &&
				fingerprint.dwmcoreImageSize != specification.requiredImageSize) ||
			(specification.requiredImageStamp &&
				fingerprint.dwmcoreImageStamp != specification.requiredImageStamp) ||
			(specification.requiredImageChecksum &&
				fingerprint.dwmcoreImageChecksum != specification.requiredImageChecksum) ||
			!EnvironmentMatches(specification.environment, fingerprint))
			return;
		++result.applicableHooks;
		if (hookManager.IsInstalled(specification.id)) {
			++result.installedHooks;
			return;
		}

		const UINT64 callSite = ResolveHookCallSite(specification);
		if (!callSite)
			return;
		++result.resolvedHooks;
		if (hookManager.Install(specification, callSite))
			++result.installedHooks;
	};
	// This image uses DDisplay. Install its verified texture adapter, not the
	// inactive Legacy path or exploratory Present candidates.
	if (win11_ddisplay::MatchesImage(fingerprint)) {
		DWM_LOG("Win11 DDisplay rendering path selected: CDDisplaySwapChain::PresentMPO");
		install(win11_ddisplay::kSpecification);
		return result;
	}
	for (const auto& specification : kHookSpecifications)
		install(specification);
	if (!result.applicableHooks)
		DWM_LOG_FORMAT(
			"No verified hook profile: OS=%lu dwmcore=%u.%u.%u.%u image=0x%lX stamp=0x%08lX checksum=0x%08lX",
			fingerprint.osBuild,
			fingerprint.dwmcoreVersion.major, fingerprint.dwmcoreVersion.minor,
			fingerprint.dwmcoreVersion.build, fingerprint.dwmcoreVersion.revision,
			fingerprint.dwmcoreImageSize,
			fingerprint.dwmcoreImageStamp, fingerprint.dwmcoreImageChecksum);
	return result;
}

} // namespace dwm_overlay
