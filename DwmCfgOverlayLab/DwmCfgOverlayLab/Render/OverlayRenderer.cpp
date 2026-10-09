#include "OverlayRenderer.h"
#include "../Common/Log.h"

#include "../IMGUI/imgui_impl_dx11.h"
#include "../IMGUI/imgui_impl_win32.h"

#include <cmath>

namespace dwm_overlay {

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kRegularFontSize = 18.0f;
constexpr float kCodeFontSize = 17.0f;
constexpr float kFontRasterizerMultiply = 1.10f;
constexpr ULONGLONG kGlyphRebuildIntervalMs = 750;
constexpr char kTechnicalGlyphSeed[] = "溢函";

constexpr ImWchar kSymbolGlyphRanges[] = {
	0x00B1, 0x00B1, // plus-minus
	0x00D7, 0x00D7, // multiplication sign
	0x00F7, 0x00F7, // division sign
	0x0391, 0x03C9, // Greek letters used by common complexity notation
	0x2000, 0x2BFF, // punctuation, arrows, math operators and common symbols
	0
};

void ConfigureCrispFont(ImFontConfig& config, bool merge = false) noexcept {
	config.MergeMode = merge;
	config.OversampleH = 2;
	config.OversampleV = 2;
	// Integer horizontal advances avoid sampling glyphs between back-buffer
	// pixels, while a small alpha boost keeps strokes readable after DWM blends
	// the translucent overlay with the desktop.
	config.PixelSnapH = true;
	config.RasterizerMultiply = kFontRasterizerMultiply;
}

void MergeSymbolFont(ImFontAtlas* fonts, float size) noexcept {
	ImFontConfig config = {};
	ConfigureCrispFont(config, true);
	fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf",
		size, &config, kSymbolGlyphRanges);
}

class D3D11OutputStateGuard final {
public:
	explicit D3D11OutputStateGuard(ID3D11DeviceContext* context) noexcept
		: context_(context) {
		context_->OMGetRenderTargets(
			D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
			renderTargets_, depthStencil_.GetAddressOf());
		context_->GetPredication(predicate_.GetAddressOf(), &predicateValue_);
		context_->SetPredication(nullptr, FALSE);
	}

	~D3D11OutputStateGuard() {
		context_->OMSetRenderTargets(
			D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
			renderTargets_, depthStencil_.Get());
		context_->SetPredication(predicate_.Get(), predicateValue_);
		for (auto& renderTarget : renderTargets_) {
			if (renderTarget) {
				renderTarget->Release();
				renderTarget = nullptr;
			}
		}
	}

private:
	ID3D11DeviceContext* context_;
	ID3D11RenderTargetView* renderTargets_[
		D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ComPtr<ID3D11DepthStencilView> depthStencil_;
	ComPtr<ID3D11Predicate> predicate_;
	BOOL predicateValue_ = FALSE;
};

} // namespace

bool OverlayRenderer::Initialize() noexcept {
	InterlockedExchange(&destroyed_, 0);
	return invalidationWorker_.Start();
}

bool OverlayRenderer::IsDestroyed() const noexcept {
	return InterlockedCompareExchange(
		const_cast<volatile LONG*>(&destroyed_), 0, 0) != 0;
}

void OverlayRenderer::SetOverlayVisible(bool visible) noexcept {
	const LONG target = visible ? 1 : 0;
	const LONG previous = InterlockedExchange(&overlayVisible_, target);
	if (previous == target)
		return;
	if (!visible) {
		// Erase the last presented UI: one invalidation makes DWM recompose
		// the covered region while rendering stays suspended.
		invalidationWorker_.QueueMovedOverlay(nullptr, false, RECT{});
		DWM_LOG("Overlay hidden by hub command; rendering suspended");
	}
	else {
		DWM_LOG("Overlay shown by hub command; rendering resumes");
	}
}

bool OverlayRenderer::IsOverlayVisible() const noexcept {
	return InterlockedCompareExchange(
		const_cast<volatile LONG*>(&overlayVisible_), 0, 0) != 0;
}

void OverlayRenderer::RequestAnswerScroll(int direction) noexcept {
	if (direction != 0)
		InterlockedExchangeAdd(&pendingAnswerScrollSteps_, direction > 0 ? 1 : -1);
}

bool OverlayRenderer::AcquireCurrentBackBuffer(
	IDXGISwapChain* swapChain,
	ComPtr<ID3D11Texture2D>& texture,
	D3D11_TEXTURE2D_DESC& description) noexcept {
	texture.Reset();
	ComPtr<IDXGISwapChain> standardSwapChain;
	IDXGISwapChain* bufferOwner = swapChain;
	if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&standardSwapChain))))
		bufferOwner = standardSwapChain.Get();

	UINT bufferIndex = 0;
	ComPtr<IDXGISwapChain3> swapChain3;
	if (SUCCEEDED(bufferOwner->QueryInterface(IID_PPV_ARGS(&swapChain3))))
		bufferIndex = swapChain3->GetCurrentBackBufferIndex();

	HRESULT result = bufferOwner->GetBuffer(
		bufferIndex, IID_PPV_ARGS(&texture));
	if (FAILED(result) && bufferIndex != 0)
		result = bufferOwner->GetBuffer(0, IID_PPV_ARGS(&texture));
	if (FAILED(result) || !texture)
		return false;

	texture->GetDesc(&description);
	if (!description.Width || !description.Height) {
		texture.Reset();
		return false;
	}
	return true;
}

bool OverlayRenderer::EnsureDevice(ID3D11Device* frameDevice) noexcept {
	if (device_ && context_ && ImGui::GetCurrentContext()) {
		if (device_.Get() != frameDevice) {
			DWM_LOG_ONCE("Ignoring swap chain from another D3D11 device");
			return false;
		}
		return true;
	}

	device_ = frameDevice;
	device_->GetImmediateContext(&context_);
	if (!context_) {
		device_.Reset();
		DWM_LOG_ONCE("D3D11 immediate context is null");
		return false;
	}

	ImGui::CreateContext();
	LoadUiFonts();
	const bool win32Ready = ImGui_ImplWin32_Init(GetDesktopWindow());
	const bool dx11Ready = ImGui_ImplDX11_Init(device_.Get(), context_.Get());
	if (!win32Ready || !dx11Ready) {
		if (dx11Ready) ImGui_ImplDX11_Shutdown();
		if (win32Ready) ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		context_.Reset();
		device_.Reset();
		DWM_LOG_ONCE("ImGui backend initialization failed");
		return false;
	}

	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	ui_.ApplyStyle();
	DWM_LOG("ImGui initialized");
	return true;
}

void OverlayRenderer::LoadUiFonts() noexcept {
	ImGuiIO& io = ImGui::GetIO();
	ImFontAtlas* fonts = io.Fonts;
	// Rasterize at the desktop's native DPI with vertical oversampling so the
	// composited glyphs stay crisp. extraGlyphRanges_ accumulates characters
	// discovered missing at runtime (dynamic glyph backfill).
	if (extraGlyphRanges_.empty()) {
		ImFontGlyphRangesBuilder builder;
		builder.AddRanges(fonts->GetGlyphRangesChineseSimplifiedCommon());
		builder.AddText(kTechnicalGlyphSeed);
		ImVector<ImWchar> ranges;
		builder.BuildRanges(&ranges);
		extraGlyphRanges_.assign(ranges.Data, ranges.Data + ranges.Size);
	}
	const UINT dpi = GetDpiForWindow(GetDesktopWindow());
	const float dpiScale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
	const ImWchar* cjkRanges = extraGlyphRanges_.data();
	const float regularSize = std::round(kRegularFontSize * dpiScale);
	const float codeSize = std::round(kCodeFontSize * dpiScale);
	ImFontConfig config = {};
	ConfigureCrispFont(config);
	ImFont* regular = fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",
		regularSize, &config, cjkRanges);
	if (regular)
		MergeSymbolFont(fonts, regularSize);
	ImFont* bold = fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyhbd.ttc",
		regularSize, &config, cjkRanges);
	if (bold)
		MergeSymbolFont(fonts, regularSize);
	ImFont* code = nullptr;
	if (regular) {
		ImFontConfig baseConfig = {};
		ConfigureCrispFont(baseConfig);
		code = fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf",
			codeSize, &baseConfig,
			fonts->GetGlyphRangesDefault());
		if (code) {
			ImFontConfig mergeConfig = {};
			ConfigureCrispFont(mergeConfig, true);
			fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",
				codeSize, &mergeConfig, cjkRanges);
			MergeSymbolFont(fonts, codeSize);
		}
	}
	if (!regular)
		regular = fonts->AddFontDefault();
	if (!bold || !bold->IsLoaded())
		bold = regular;
	if (!code || !code->IsLoaded())
		code = regular;
	fontRegular_ = regular;
	ui_.SetFonts(regular, bold, code);
}

bool OverlayRenderer::RebuildUiFonts() noexcept {
	const ULONGLONG startedAt = GetTickCount64();
	ImGuiIO& io = ImGui::GetIO();
	ImGui_ImplDX11_InvalidateDeviceObjects();
	io.Fonts->Clear();
	LoadUiFonts();
	if (!io.Fonts->Build()) {
		DWM_LOG("Extended font atlas build failed");
		return false;
	}
	if (!ImGui_ImplDX11_CreateDeviceObjects()) {
		DWM_LOG("Extended font atlas upload failed");
		return false;
	}
	// Cached draw data contains UVs from the previous atlas. Force a fresh UI
	// frame before it can be rendered with the replacement texture.
	nextFrameQpc_ = 0;
	DWM_LOG_FORMAT("Font atlas ready in %llu ms (%d x %d)",
		static_cast<unsigned long long>(GetTickCount64() - startedAt),
		io.Fonts->TexWidth, io.Fonts->TexHeight);
	return true;
}

void OverlayRenderer::ScanMissingGlyphs(
	UINT64 answerEpoch, UINT64 textLength, bool forceRebuild) noexcept {
	if (!fontRegular_ || glyphScanBuffer_.empty())
		return;
	if (answerEpoch != glyphScanEpoch_) {
		glyphScanEpoch_ = answerEpoch;
		glyphScannedUpTo_ = 0;
		pendingMissingGlyphs_.clear();
	}
	else if (textLength < glyphScannedUpTo_) {
		glyphScannedUpTo_ = 0; // answer was cleared or replaced
		pendingMissingGlyphs_.clear();
	}

	if (textLength != glyphScannedUpTo_) {
		const char* text = glyphScanBuffer_.data();
		const char* end = text + textLength;
		const char* cursor = text + glyphScannedUpTo_;
		while (cursor < end) {
			unsigned codepoint = static_cast<unsigned char>(*cursor);
			int length = 1;
			if (codepoint >= 0x80) {
				if ((codepoint & 0xE0) == 0xC0 && cursor + 1 < end) {
					codepoint = ((codepoint & 0x1F) << 6) | (cursor[1] & 0x3F);
					length = 2;
				}
				else if ((codepoint & 0xF0) == 0xE0 && cursor + 2 < end) {
					codepoint = ((codepoint & 0x0F) << 12) |
						((cursor[1] & 0x3F) << 6) | (cursor[2] & 0x3F);
					length = 3;
				}
				else if ((codepoint & 0xF8) == 0xF0 && cursor + 3 < end) {
					codepoint = ((codepoint & 0x07) << 18) |
						((cursor[1] & 0x3F) << 12) |
						((cursor[2] & 0x3F) << 6) | (cursor[3] & 0x3F);
					length = 4;
				}
				else {
					break; // Keep the partial UTF-8 sequence for the next frame.
				}
			}
			if (codepoint >= 0x80 && codepoint <= 0xFFFF &&
				fontRegular_->FindGlyphNoFallback(
					static_cast<ImWchar>(codepoint)) == nullptr) {
				pendingMissingGlyphs_.append(
					cursor, static_cast<size_t>(length));
			}
			cursor += length;
		}
		glyphScannedUpTo_ = static_cast<UINT64>(cursor - text);
	}

	if (pendingMissingGlyphs_.empty())
		return;
	const ULONGLONG now = GetTickCount64();
	if (!forceRebuild && lastGlyphRebuildTick_ != 0 &&
		now - lastGlyphRebuildTick_ < kGlyphRebuildIntervalMs)
		return; // Batch streamed glyphs without leaving fallback boxes visible.
	lastGlyphRebuildTick_ = now;

	ImGuiIO& io = ImGui::GetIO();
	ImFontGlyphRangesBuilder builder;
	builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
	if (!extraGlyphRanges_.empty())
		builder.AddRanges(extraGlyphRanges_.data());
	builder.AddText(pendingMissingGlyphs_.c_str());
	ImVector<ImWchar> builtRanges;
	builder.BuildRanges(&builtRanges);
	const std::vector<ImWchar> previousRanges = extraGlyphRanges_;
	extraGlyphRanges_.assign(
		builtRanges.Data, builtRanges.Data + builtRanges.Size);
	if (RebuildUiFonts()) {
		pendingMissingGlyphs_.clear();
		DWM_LOG("Font atlas rebuilt with extended glyph ranges");
		return;
	}

	// Keep the overlay usable if an unexpectedly large range exceeds the
	// device's atlas limits. The missing text remains queued for a later retry.
	extraGlyphRanges_ = previousRanges;
	if (RebuildUiFonts())
		DWM_LOG("Extended font atlas rejected; previous atlas restored");
	else
		DWM_LOG("Font atlas fallback failed");
}

bool OverlayRenderer::GetOverlayBounds(
	const ImVec2& position,
	const ImVec2& size,
	UINT width,
	UINT height,
	RECT& bounds) noexcept {
	if (size.x <= 0.0f || size.y <= 0.0f)
		return false;
	LONG left = static_cast<LONG>(position.x) - 3;
	LONG top = static_cast<LONG>(position.y) - 3;
	LONG right = static_cast<LONG>(position.x + size.x + 4.0f);
	LONG bottom = static_cast<LONG>(position.y + size.y + 4.0f);
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > static_cast<LONG>(width)) right = static_cast<LONG>(width);
	if (bottom > static_cast<LONG>(height)) bottom = static_cast<LONG>(height);
	if (right <= left || bottom <= top)
		return false;
	bounds = { left, top, right, bottom };
	return true;
}

bool OverlayRenderer::ShouldBuildFrame(UINT width, UINT height) noexcept {
	LARGE_INTEGER now = {};
	if (!QueryPerformanceCounter(&now))
		return true;
	if (!qpcFrequency_) {
		LARGE_INTEGER frequency = {};
		if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
			return true;
		qpcFrequency_ = frequency.QuadPart;
	}
	const LONGLONG interval =
		(qpcFrequency_ + kTargetFps - 1) / kTargetFps;
	const bool sizeChanged = frameWidth_ != width || frameHeight_ != height;
	if (!nextFrameQpc_ || sizeChanged) {
		frameWidth_ = width;
		frameHeight_ = height;
		nextFrameQpc_ = now.QuadPart + interval;
		DWM_LOG_ONCE("ImGui logic frame rate limited to 60 FPS");
		return true;
	}
	if (now.QuadPart < nextFrameQpc_)
		return false;
	if (now.QuadPart - nextFrameQpc_ > interval * 4)
		nextFrameQpc_ = now.QuadPart + interval;
	else {
		do {
			nextFrameQpc_ += interval;
		} while (nextFrameQpc_ <= now.QuadPart);
	}
	return true;
}

void OverlayRenderer::LogSourceOnce(const HookSpec& source) noexcept {
	const LONG bit = 1L << (static_cast<UINT32>(source.id) & 31);
	const LONG previous = InterlockedOr(&sourceLogMask_, bit);
	if ((previous & bit) == 0)
		DWM_LOG_FORMAT("Present source active: %s", source.name);
}

void OverlayRenderer::Render(
	void* presentationObject,
	const HookSpec& source) noexcept {
	if (!presentationObject || IsDestroyed() ||
		InterlockedCompareExchange(&overlayVisible_, 0, 0) == 0 ||
		InterlockedCompareExchange(&presentBusy_, 1, 0) != 0)
		return;
	__try {
		RenderFrame(presentationObject, source);
	}
	__finally {
		InterlockedExchange(&presentBusy_, 0);
	}
}

void OverlayRenderer::RenderTexture(ID3D11Texture2D* texture, const HookSpec& source,
	UINT64 chainIdentity, ddisplay::IDisplayScanoutDirty* scanout, UINT32 plane) noexcept {
	if (!texture || !scanout || IsDestroyed() ||
		InterlockedCompareExchange(&overlayVisible_, 0, 0) == 0 ||
		InterlockedCompareExchange(&presentBusy_, 1, 0) != 0)
		return;
	__try {
		RenderTextureFrame(texture, source, nullptr, chainIdentity, scanout, plane);
	}
	__finally {
		InterlockedExchange(&presentBusy_, 0);
	}
}

void OverlayRenderer::ClearDisplayBackdrops() noexcept {
	for (auto& slot : displayBackdrops_) {
		slot.compositor.Shutdown();
		slot.texture.Reset();
	}
	displayChainIdentity_ = 0;
	displayDescription_ = {};
}

BackdropCompositor* OverlayRenderer::SelectDisplayBackdrop(ID3D11Texture2D* texture,
	const D3D11_TEXTURE2D_DESC& desc, UINT64 chainIdentity) noexcept {
	if (displayChainIdentity_ != chainIdentity || displayDescription_.Width != desc.Width ||
		displayDescription_.Height != desc.Height || displayDescription_.Format != desc.Format) {
		ClearDisplayBackdrops();
		displayChainIdentity_ = chainIdentity;
		displayDescription_ = desc;
	}
	for (auto& slot : displayBackdrops_)
		if (slot.texture.Get() == texture)
			return &slot.compositor;
	for (auto& slot : displayBackdrops_) {
		if (!slot.texture) {
			slot.texture = texture;
			return &slot.compositor;
		}
	}
	// Retaining four rotating buffers is bounded. Do not evict a dirty buffer
	// and lose its clean background; a chain/resize change resets this cache.
	DWM_LOG_ONCE("DDisplay rotating-buffer cache exhausted; additional texture skipped");
	return nullptr;
}

void OverlayRenderer::RenderFrame(
	void* presentationObject,
	const HookSpec& source) noexcept {
	LogSourceOnce(source);

	auto swapChain = reinterpret_cast<IDXGISwapChain*>(presentationObject);
	ComPtr<ID3D11Texture2D> backBuffer;
	D3D11_TEXTURE2D_DESC backBufferDescription = {};
	if (!AcquireCurrentBackBuffer(swapChain, backBuffer,
		backBufferDescription)) {
		DWM_LOG_ONCE("Unable to acquire current swap-chain buffer");
		return;
	}

	RenderTextureFrame(backBuffer.Get(), source, swapChain, 0, nullptr, 0);
}

void OverlayRenderer::RenderTextureFrame(ID3D11Texture2D* backBuffer, const HookSpec& source,
	IDXGISwapChain* swapChain, UINT64 chainIdentity,
	ddisplay::IDisplayScanoutDirty* scanout, UINT32 plane) noexcept {
	LogSourceOnce(source);
	D3D11_TEXTURE2D_DESC backBufferDescription = {};
	backBuffer->GetDesc(&backBufferDescription);
	ComPtr<ID3D11Device> frameDevice;
	backBuffer->GetDevice(&frameDevice);
	if (!frameDevice || !EnsureDevice(frameDevice.Get())) {
		DWM_LOG_ONCE("Back buffer device is unavailable");
		return;
	}
	// Initialize/validate the device before selecting per-device backdrop state.
	BackdropCompositor* compositor = &backdropCompositor_;
	if (scanout) {
		compositor = SelectDisplayBackdrop(backBuffer, backBufferDescription, chainIdentity);
		if (!compositor) return;
	}

	ComPtr<ID3D11RenderTargetView> frameRenderTarget;
	if (FAILED(device_->CreateRenderTargetView(
		backBuffer, nullptr, &frameRenderTarget))) {
		DWM_LOG_ONCE("CreateRenderTargetView failed");
		return;
	}

	// Incrementally backfill glyphs for characters missing from the font.
	if (answerProvider_) {
		constexpr UINT64 kScanCapacity = 512 * 1024 + 2;
		if (glyphScanBuffer_.size() < kScanCapacity)
			glyphScanBuffer_.resize(static_cast<size_t>(kScanCapacity));
		const AnswerSnapshot answer = answerProvider_->CopySnapshot(
			glyphScanBuffer_.data(), glyphScanBuffer_.size(),
			nullptr, 0, nullptr, 0);
		ScanMissingGlyphs(answer.epoch, answer.answerLength,
			answer.state == AnswerState::Completed);
	}

	if (ShouldBuildFrame(backBufferDescription.Width,
		backBufferDescription.Height)) {
		ImGuiIO& io = ImGui::GetIO();
		inputSource_.Update(io);
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		io.DisplaySize = ImVec2(
			static_cast<float>(backBufferDescription.Width),
			static_cast<float>(backBufferDescription.Height));
		ImGui::NewFrame();

		const int scrollSteps = static_cast<int>(
			InterlockedExchange(&pendingAnswerScrollSteps_, 0));
		const UiFrameResult uiResult = ui_.Build(
			uiState_.Read(), answerProvider_, scrollSteps);
		ImGui::Render();
		hasCachedOverlayRect_ = GetOverlayBounds(
			uiResult.windowPosition, uiResult.windowSize,
			backBufferDescription.Width, backBufferDescription.Height,
			cachedOverlayRect_);
	}

	const bool drawOverlay = hasCachedOverlayRect_;
	if (scanout && drawOverlay && FAILED(ddisplay::SetFullPlaneDirtyRects(
		scanout, plane, backBufferDescription.Width, backBufferDescription.Height))) {
		DWM_LOG_ONCE("DDisplay full-plane dirty-rect update failed; drawing skipped");
		return;
	}
	{
		// The translucent UI must blend over the freshest composition. DWM's
		// partial recomposition of the (per-frame invalidated) region is not
		// guaranteed to land on every frame, so the compositor compares the
		// region against the previous final output on the GPU: unchanged
		// pixels mean our own UI is still there and the saved backdrop is
		// restored; changed pixels mean DWM recomposed and the fresh content
		// becomes the backdrop. Neither ghosts nor alpha accumulation are
		// possible in either case.
		D3D11OutputStateGuard stateGuard(context_.Get());
		bool restored = false;
		if (drawOverlay) {
			restored = compositor->EnsureSize(
				device_.Get(), backBufferDescription);
			if (restored)
				compositor->CaptureRegion(
					context_.Get(), backBuffer, cachedOverlayRect_);
			if (restored)
				restored = compositor->RestoreBackdrop(
					context_.Get(), backBuffer, cachedOverlayRect_);
			if (!restored && scanout) {
				DWM_LOG_ONCE("DDisplay background preservation unavailable; drawing skipped");
				return; // Never alpha-accumulate onto a rotating native surface.
			}
			if (!restored)
				DWM_LOG_ONCE(
					"Backdrop compositor unavailable; drawing without restore");

			ID3D11RenderTargetView* target = frameRenderTarget.Get();
			context_->OMSetRenderTargets(1, &target, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			if (restored)
				compositor->SaveOutput(
					context_.Get(), backBuffer, cachedOverlayRect_);
		}
		if (scanout) {
			// Native adapter accepts only the unscaled primary desktop plane.
			invalidationWorker_.QueueScreenOverlay(drawOverlay, cachedOverlayRect_);
			context_->Flush(); // Submit our writes before ExecutePresent signals/submits.
		}
		else
			invalidationWorker_.QueueMovedOverlay(swapChain,
				drawOverlay && hasCachedOverlayRect_, cachedOverlayRect_);
	}
}

void OverlayRenderer::ClearDeviceResources() noexcept {
	if (ImGui::GetCurrentContext()) {
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}
	backdropCompositor_.Shutdown();
	ClearDisplayBackdrops();
	hasCachedOverlayRect_ = false;
	context_.Reset();
	device_.Reset();
}

void OverlayRenderer::Shutdown() noexcept {
	if (InterlockedExchange(&destroyed_, 1) != 0)
		return;
	// A present may already be inside RenderFrame on the compositor thread;
	// wait for it to leave before tearing down the resources it touches.
	const ULONGLONG deadline = GetTickCount64() + 3000;
	while (InterlockedCompareExchange(&presentBusy_, 0, 0) != 0 &&
		GetTickCount64() < deadline)
		Sleep(1);
	invalidationWorker_.QueueScreenOverlay(false, RECT{});
	ClearDeviceResources();
	invalidationWorker_.Stop();
	DWM_LOG("ImGui overlay destroyed");
}

} // namespace dwm_overlay
