#include "FrameRouter.h"
#include "OverlayRenderer.h"
#include "../Common/Log.h"

namespace dwm_overlay {

void FrameRouter::Route(const HookInvocation& invocation) noexcept {
	if (!invocation.specification || !invocation.argument)
		return;

	// Every current profile resolves to an IDXGISwapChain-compatible object.
	// Future DComp surface or MPO-plane hooks can be normalized here by a new
	// adapter without changing HookManager or OverlayRenderer.
	switch (invocation.specification->kind) {
	case PresentKind::DxgiPresent:
	case PresentKind::DxgiPresent1:
	case PresentKind::MultiplaneOverlay:
	case PresentKind::VmwarePresentInternal:
		renderer_.Render(invocation.argument, *invocation.specification);
		break;
	default:
		DWM_LOG_ONCE("Unsupported frame-target adapter");
		break;
	}
}

} // namespace dwm_overlay
