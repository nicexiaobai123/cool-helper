#pragma once
#include <Windows.h>
#include <array>
#include <memory>
#include "FrameTarget.h"
#include "InvalidationWorker.h"
#include "RenderSession.h"
#include "../IPC/UiState.h"

namespace dwm_overlay {
// Policy/lifecycle owner. No private presentation interface or version-specific
// resource acquisition belongs here; each display/device owns one UI session.
class OverlayRenderer final {
public:
    bool Initialize() noexcept;
    void Render(const FrameTarget& frame, const HookSpec& source) noexcept;
    void Shutdown() noexcept;
    bool IsDestroyed() const noexcept;
    void SetOverlayVisible(bool visible) noexcept;
    bool IsOverlayVisible() const noexcept;
    void SetDisplayMode(DisplayMode mode) noexcept;
    DisplayMode GetDisplayMode() const noexcept;
    void SetCompatibilityDefault(DisplayMode mode) noexcept;
    void RequestAnswerScroll(int direction) noexcept;
    UiStateStore& StateStore() noexcept { return uiState_; }
    void SetAnswerProvider(IAnswerProvider* provider) noexcept { answerProvider_ = provider; }
private:
    void RenderFrame(const FrameTarget& frame, const HookSpec& source) noexcept;
    void PruneSessions() noexcept;
    struct Session {
        UINT64 key = 0;
        DisplayTarget display = {};
        D3D11_TEXTURE2D_DESC description = {};
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        std::unique_ptr<RenderSession> renderer;
        LONG64 scrollPosition = 0;
    };
    std::array<Session, 8> sessions_;
    volatile LONG presentBusy_ = 0, destroyed_ = 0, visible_ = 1;
    volatile LONG displayMode_ = static_cast<LONG>(DisplayMode::Compatible);
    volatile LONG compatibilityDefault_ = static_cast<LONG>(DisplayMode::PrimaryOnly);
    volatile LONG64 scrollPosition_ = 0;
    ULONGLONG nextPrune_ = 0;
    InvalidationWorker invalidationWorker_;
    UiStateStore uiState_;
    IAnswerProvider* answerProvider_ = nullptr;
};
} // namespace dwm_overlay
