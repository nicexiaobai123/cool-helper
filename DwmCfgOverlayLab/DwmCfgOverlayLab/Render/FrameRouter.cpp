#include "FrameRouter.h"
#include "OverlayRenderer.h"
#include "../Common/Log.h"

namespace dwm_overlay {
void FrameRouter::Route(const HookInvocation& invocation) noexcept {
    if (!invocation.specification || !invocation.argument ||
        !renderer_.NeedsFrameProcessing()) return;
    FrameTarget frame;
    switch (invocation.specification->kind) {
    case PresentKind::DDisplayMultiplaneOverlay:
        if (!invocation.cpuContext ||
            !displayAdapter_.Acquire(*invocation.cpuContext, topology_, frame)) return;
        break;
    case PresentKind::DxgiPresent:
    case PresentKind::DxgiPresent1:
    case PresentKind::MultiplaneOverlay:
    case PresentKind::VmwarePresentInternal:
        if (!dxgiAdapter_.Acquire(invocation.argument, frame)) {
            DWM_LOG_ONCE("Unable to acquire DXGI frame target"); return;
        }
        break;
    default:
        DWM_LOG_ONCE("Unsupported frame-target adapter"); return;
    }
    renderer_.Render(frame, *invocation.specification);
}
} // namespace dwm_overlay
