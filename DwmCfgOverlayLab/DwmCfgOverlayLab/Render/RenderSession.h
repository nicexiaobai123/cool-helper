#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <string>
#include <array>
#include <vector>

#include "../Hooks/HookTypes.h"
#include "../IPC/UiState.h"
#include "../Input/InputSource.h"
#include "../UI/OverlayUi.h"
#include "BackdropCompositor.h"
#include "FrameTarget.h"

namespace dwm_overlay {

class RenderSession final {
public:
    bool Render(const FrameTarget& frame, const HookSpec& source,
        const UiSnapshot& snapshot, IAnswerProvider* answers, int scrollSteps, RECT& screenRect) noexcept;
    void Shutdown() noexcept;
    RenderSession() = default;
    ~RenderSession() { Shutdown(); }
    RenderSession(const RenderSession&) = delete;
    RenderSession& operator=(const RenderSession&) = delete;

private:
	// Scans newly appended answer text for characters missing from the font
	// and rebuilds the atlas with extended ranges (throttled).
	void ScanMissingGlyphs(
		UINT64 answerEpoch, UINT64 textLength, bool forceRebuild) noexcept;
	static constexpr UINT kTargetFps = 60;

	bool EnsureDevice(ID3D11Device* frameDevice) noexcept;
	void LoadUiFonts() noexcept;
	bool RebuildUiFonts() noexcept;
	bool ShouldBuildFrame(UINT width, UINT height) noexcept;
	static bool GetOverlayBounds(
		const ImVec2& position,
		const ImVec2& size,
		UINT width,
		UINT height,
		RECT& bounds) noexcept;
	void ClearDeviceResources() noexcept;
    bool RenderFrame(const FrameTarget& frame, const UiSnapshot& snapshot,
        IAnswerProvider* answers, int scrollSteps, RECT& screenRect) noexcept;
    BackdropCompositor* SelectBackdrop(const FrameTarget& frame) noexcept;
    void ClearBackdrops() noexcept;
	void LogSourceOnce(const HookSpec& source) noexcept;

    ImGuiContext* imguiContext_ = nullptr;
    float fontScale_ = 1.0f;
    ULONGLONG nextDpiCheck_ = 0;
    volatile LONG sourceLogMask_ = 0;
	Microsoft::WRL::ComPtr<ID3D11Device> device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
	LONGLONG qpcFrequency_ = 0;
	LONGLONG nextFrameQpc_ = 0;
	UINT frameWidth_ = 0;
	UINT frameHeight_ = 0;
	RECT cachedOverlayRect_ = {};
	bool hasCachedOverlayRect_ = false;
    struct BackdropSlot {
        UINT64 chain = 0, resource = 0, generation = 0;
        BackdropCompositor compositor;
    };
    std::array<BackdropSlot, 8> backdrops_;
	IAnswerProvider* answerProvider_ = nullptr;
	ImFont* fontRegular_ = nullptr;
	std::vector<ImWchar> extraGlyphRanges_;
	std::string pendingMissingGlyphs_;
	std::vector<char> glyphScanBuffer_;
	UINT64 glyphScanEpoch_ = 0;
	UINT64 glyphScannedUpTo_ = 0;
	ULONGLONG lastGlyphRebuildTick_ = 0;
	DesktopPollingInputSource inputSource_;
	OverlayUi ui_;
};

} // namespace dwm_overlay
