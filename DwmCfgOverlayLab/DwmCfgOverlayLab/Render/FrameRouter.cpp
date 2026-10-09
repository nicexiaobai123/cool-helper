#include "FrameRouter.h"
#include "OverlayRenderer.h"
#include "../Common/Log.h"

namespace dwm_overlay {

void FrameRouter::Route(const HookInvocation& invocation) noexcept {
	if (!invocation.specification || !invocation.argument)
		return;

	// Native DDisplay buffers are not IDXGISwapChain-compatible.
	switch (invocation.specification->kind) {
	case PresentKind::DDisplayMultiplaneOverlay: {
		if (!invocation.cpuContext || renderer_.IsDestroyed() || !renderer_.IsOverlayVisible())
			return;
		DDisplayFrame frame;
		if (!displayAdapter_.Acquire(*invocation.cpuContext, frame))
			return;
		renderer_.RenderTexture(frame.texture.Get(), *invocation.specification,
			invocation.cpuContext->rcx, frame.dirtyScanout.Get(), frame.planeIndex);
		break;
	}
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
