#pragma once

#include "../Hooks/HookTypes.h"
#include "../Platform/SystemProbe.h"

namespace dwm_overlay {
namespace win11_ddisplay {

// PDB: CDDisplaySwapChain::PresentMPO (RVA 140910), immediately before
// CDDisplaySwapChain::ExecutePresent (RVA 2BD2C4). This is NOT its entry.
// RCX = internal object base (entry this - 18h), RDX = IDisplayScanout*,
// R8 = const DWM_PRESENT_MULTIPLANE_OVERLAY*, R9D = plane count.
// Evidence: dwmcore SHA256 40579ECFA394395677FEC84014C32BA5495F334B6D96C5653FD5CAA129DC1222;
// PDB GUID 5F9A6234-BFC6-EC6A-1170-91D25FF7CF22, age 1.
static const PatternVariant kVariants[] = {
	{
		{
			"\x48\x89\x5C\x24\x10\x48\x89\x6C\x24\x18\x56\x57\x41\x54\x41\x56"
			"\x41\x57\x48\x83\xEC\x40\x45\x8B",
			"xxxxxxxxxxxxxxxxxxxxxxxx"
		},
		0xD9, 0x1D, 0, true, "CDDisplaySwapChain::PresentMPO/D3D11 (26100.9278)",
		{
			"\xD5\x00\x00\x00\x44\x8B\xCD\x4C\x8B\xC6\x48\x8B\xD3\x49\x8B\xCE"
			"\xE8\x00\x00\x00\x00\x8B\xF8\x85\xC0\x0F\x88\xFE\x00",
			"xxxxxxxxxxxxxxxxx????xxxxxxxx"
		},
		16
	}
};

constexpr HookSpec MakeSpecification() noexcept {
	HookSpec spec = {
		HookSiteId::Win11DDisplayPresentMpo,
		"CDDisplaySwapChain::PresentMPO/D3D11",
		L"dwmcore.dll", 26200, 26200, 26100, 26100,
		EnvironmentRequirement::Any, ArgumentSource::R8,
		PresentKind::DDisplayMultiplaneOverlay, kVariants, _countof(kVariants),
		CallSiteEncoding::RelativeCallToVerifiedLocal
	};
	// Internal object ABI is verified only for this exact image.
	spec.minimumModuleRevision = spec.maximumModuleRevision = 9278;
	spec.requiredImageSize = 0x443000;
	spec.requiredImageStamp = 0x9A1AF3BA;
	spec.requiredImageChecksum = 0x004477E9;
	spec.localTargetPattern = {
		"\x48\x89\x5C\x24\x20\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57"
		"\x48\x8D\x6C\x24\xD9\x48\x81\xEC\xF0\x00\x00\x00\x48\x8B\x05\x99",
		"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
	};
	return spec;
}

static constexpr HookSpec kSpecification = MakeSpecification();

inline bool MatchesImage(const SystemFingerprint& fingerprint) noexcept {
	return fingerprint.osBuild == 26200 &&
		fingerprint.dwmcoreVersion.major == 10 &&
		fingerprint.dwmcoreVersion.minor == 0 &&
		fingerprint.dwmcoreVersion.build == 26100 &&
		fingerprint.dwmcoreVersion.revision == 9278 &&
		fingerprint.dwmcoreImageSize == kSpecification.requiredImageSize &&
		fingerprint.dwmcoreImageStamp == kSpecification.requiredImageStamp &&
		fingerprint.dwmcoreImageChecksum == kSpecification.requiredImageChecksum;
}

} // namespace win11_ddisplay
} // namespace dwm_overlay
