#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "../Hooks/HookTypes.h"
#include "../IPC/UiState.h"
#include "../Input/InputSource.h"
#include "../UI/OverlayUi.h"
#include "BackdropCompositor.h"
#include "InvalidationWorker.h"

namespace dwm_overlay {

class OverlayRenderer final {
public:
	bool Initialize() noexcept;
	void Render(void* presentationObject, const HookSpec& source) noexcept;
	void Shutdown() noexcept;
	bool IsDestroyed() const noexcept;
	// Hub-driven visibility (control IPC). Hide erases the last presented UI
	// by invalidating the region once; show simply resumes rendering.
	void SetOverlayVisible(bool visible) noexcept;
	bool IsOverlayVisible() const noexcept;
	// May be called by the control IPC worker. The render thread consumes the
	// accumulated steps before building the next ImGui frame.
	void RequestAnswerScroll(int direction) noexcept;
	UiStateStore& StateStore() noexcept { return uiState_; }
	void SetAnswerProvider(IAnswerProvider* provider) noexcept {
		answerProvider_ = provider;
	}

private:
	// Scans newly appended answer text for characters missing from the font
	// and rebuilds the atlas with extended ranges (throttled).
	void ScanMissingGlyphs(
		UINT64 answerEpoch, UINT64 textLength, bool forceRebuild) noexcept;
	static constexpr UINT kTargetFps = 60;

	bool AcquireCurrentBackBuffer(
		IDXGISwapChain* swapChain,
		Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
		D3D11_TEXTURE2D_DESC& description) noexcept;
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
	void RenderFrame(void* presentationObject, const HookSpec& source) noexcept;
	void LogSourceOnce(const HookSpec& source) noexcept;

	volatile LONG presentBusy_ = 0;
	volatile LONG destroyed_ = 0;
	volatile LONG sourceLogMask_ = 0;
	volatile LONG overlayVisible_ = 1;
	volatile LONG pendingAnswerScrollSteps_ = 0;
	Microsoft::WRL::ComPtr<ID3D11Device> device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
	LONGLONG qpcFrequency_ = 0;
	LONGLONG nextFrameQpc_ = 0;
	UINT frameWidth_ = 0;
	UINT frameHeight_ = 0;
	RECT cachedOverlayRect_ = {};
	bool hasCachedOverlayRect_ = false;
	InvalidationWorker invalidationWorker_;
	BackdropCompositor backdropCompositor_;
	UiStateStore uiState_;
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
