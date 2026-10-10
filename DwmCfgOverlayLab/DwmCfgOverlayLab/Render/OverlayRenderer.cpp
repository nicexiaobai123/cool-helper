#include "OverlayRenderer.h"
#include "../Common/Log.h"
#include <new>

namespace dwm_overlay {
bool OverlayRenderer::Initialize() noexcept {
    InterlockedExchange(&destroyed_, 0);
    return invalidationWorker_.Start();
}
bool OverlayRenderer::IsDestroyed() const noexcept {
    return InterlockedCompareExchange(const_cast<volatile LONG*>(&destroyed_), 0, 0) != 0;
}
bool OverlayRenderer::IsOverlayVisible() const noexcept {
    return InterlockedCompareExchange(const_cast<volatile LONG*>(&visible_), 0, 0) != 0;
}
bool OverlayRenderer::NeedsFrameProcessing() const noexcept {
    return !IsDestroyed() && (IsOverlayVisible() ||
        InterlockedCompareExchange(const_cast<volatile LONG*>(&cleanupPending_), 0, 0) != 0);
}
void OverlayRenderer::SetOverlayVisible(bool visible) noexcept {
    if (InterlockedExchange(&visible_, visible ? 1 : 0) == (visible ? 1 : 0)) return;
    if (!visible) {
        InterlockedExchange(&cleanupPending_, 1);
        invalidationWorker_.EraseAll();
    } else invalidationWorker_.QueueFrameWake();
    DWM_LOG(visible ? "Overlay shown by hub command" : "Overlay hidden by hub command");
}
void OverlayRenderer::SetCompatibilityDefault(DisplayMode mode) noexcept {
    if (mode != DisplayMode::Compatible)
        InterlockedExchange(&compatibilityDefault_, static_cast<LONG>(mode));
}
void OverlayRenderer::SetDisplayMode(DisplayMode mode) noexcept {
    if (!coolhelper_overlay::IsDisplayMode(static_cast<UINT32>(mode))) return;
    if (InterlockedExchange(&displayMode_, static_cast<LONG>(mode)) == static_cast<LONG>(mode)) return;
    invalidationWorker_.EraseAll();
    DWM_LOG_FORMAT("Overlay display mode changed: %u", static_cast<UINT32>(mode));
}
DisplayMode OverlayRenderer::GetDisplayMode() const noexcept {
    return static_cast<DisplayMode>(InterlockedCompareExchange(
        const_cast<volatile LONG*>(&displayMode_), 0, 0));
}
void OverlayRenderer::RequestAnswerScroll(int direction) noexcept {
    if (direction) InterlockedExchangeAdd64(&scrollPosition_, direction > 0 ? 1 : -1);
}
void OverlayRenderer::PruneSessions() noexcept {
    if (GetTickCount64() < nextPrune_) return;
    nextPrune_ = GetTickCount64() + 2000;
    for (auto& session : sessions_) {
        if (!session.renderer) continue;
        DisplayTarget current;
        const bool removed = session.display.monitor &&
            !DescribeMonitor(session.display.monitor, current);
        if (removed || FAILED(session.device->GetDeviceRemovedReason())) {
            invalidationWorker_.QueueTarget(session.key, false, RECT{});
            session.renderer.reset();
            session.device.Reset();
            session.key = 0;
        }
    }
}
void OverlayRenderer::Render(const FrameTarget& frame, const HookSpec& source) noexcept {
    if (!frame.texture || !NeedsFrameProcessing() ||
        InterlockedCompareExchange(&presentBusy_, 1, 0) != 0) return;
    __try { RenderFrame(frame, source); RefreshCleanupState(); }
    __finally { InterlockedExchange(&presentBusy_, 0); }
}
void OverlayRenderer::RefreshCleanupState() noexcept {
    if (IsOverlayVisible()) return;
    for (const auto& session : sessions_) {
        if (session.renderer && session.renderer->HasOverlayOutput()) return;
    }
    if (InterlockedExchange(&cleanupPending_, 0))
        DWM_LOG("Overlay cleanup complete; hidden frame processing idle");
}
void OverlayRenderer::RenderFrame(const FrameTarget& frame, const HookSpec& source) noexcept {
    const DisplayMode mode = GetDisplayMode();
    const auto fallback = static_cast<DisplayMode>(
        InterlockedCompareExchange(&compatibilityDefault_, 0, 0));
    const bool drawUi = IsOverlayVisible() && AllowsDisplay(frame.display, mode, fallback);
    PruneSessions();
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    frame.texture->GetDevice(&device);
    if (!device) return;
    Session* selected = nullptr;
    for (auto& session : sessions_) {
        if (session.renderer && session.key == frame.DisplayKey() && session.device.Get() == device.Get()) {
            selected = &session; break;
        }
    }
    // A hidden or excluded target still needs a cleanup pass on each buffer
    // we previously drew into. Never create new sessions for cleanup.
    if (!drawUi) {
        RECT erased = {};
        if (selected && selected->renderer->RestoreWithoutUi(frame, erased))
            invalidationWorker_.QueueCleanupRect(erased);
        return;
    }
    if (!selected) {
        for (auto& session : sessions_) if (!session.renderer) { selected = &session; break; }
    }
    if (!selected) {
        DWM_LOG_ONCE("Display/device session limit reached; extra target skipped");
        return;
    }
    const bool sizeChanged = selected->description.Width != frame.description.Width ||
        selected->description.Height != frame.description.Height ||
        selected->description.Format != frame.description.Format;
    const bool moved = !EqualRect(&selected->display.desktop, &frame.display.desktop);
    if (selected->renderer && (sizeChanged || moved)) {
        invalidationWorker_.QueueTarget(selected->key, false, RECT{});
        selected->renderer.reset();
    }
    const LONG64 scroll = InterlockedCompareExchange64(&scrollPosition_, 0, 0);
    if (!selected->renderer) {
        selected->renderer.reset(new (std::nothrow) RenderSession());
        if (!selected->renderer) return;
        selected->key = frame.DisplayKey();
        selected->device = device;
        selected->scrollPosition = scroll;
        DWM_LOG_FORMAT("Render session created: display=%p primary=%u size=%ux%u device=%p",
            frame.display.monitor, frame.display.primary ? 1 : 0,
            frame.description.Width, frame.description.Height, device.Get());
    }
    selected->description = frame.description;
    selected->display = frame.display;
    const LONG64 delta = scroll - selected->scrollPosition;
    const int steps = static_cast<int>(delta > 256 ? 256 : delta < -256 ? -256 : delta);
    selected->scrollPosition = scroll;
    RECT screen = {};
    if (selected->renderer->Render(frame, source, uiState_.Read(), answerProvider_, steps, screen))
        invalidationWorker_.QueueTarget(selected->key, true, screen);
    // A control command can race an in-flight frame. Erase its just-drawn region
    // too, not only regions tracked when the IPC worker received the command.
    if (!IsOverlayVisible() || !AllowsDisplay(frame.display, GetDisplayMode(), fallback)) {
        RECT erased = {};
        if (selected->renderer->RestoreWithoutUi(frame, erased))
            invalidationWorker_.QueueCleanupRect(erased);
        invalidationWorker_.EraseAll();
    } else if (mode != GetDisplayMode()) invalidationWorker_.EraseAll();
}
void OverlayRenderer::Shutdown() noexcept {
    if (InterlockedExchange(&destroyed_, 1)) return;
    // Runtime restores hooks and drains callbacks before calling Shutdown.
    invalidationWorker_.EraseAll();
    for (auto& session : sessions_) {
        session.renderer.reset();
        session.device.Reset();
        session.key = 0;
    }
    invalidationWorker_.Stop();
    DWM_LOG("ImGui overlay destroyed");
}
} // namespace dwm_overlay
