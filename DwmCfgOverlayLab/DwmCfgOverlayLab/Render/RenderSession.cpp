#include "RenderSession.h"
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

bool RenderSession::EnsureDevice(ID3D11Device* frameDevice) noexcept {
	if (device_ && context_ && imguiContext_) {
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

	imguiContext_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imguiContext_);
	LoadUiFonts();
	const bool win32Ready = ImGui_ImplWin32_Init(GetDesktopWindow());
	const bool dx11Ready = ImGui_ImplDX11_Init(device_.Get(), context_.Get());
	if (!win32Ready || !dx11Ready) {
		if (dx11Ready) ImGui_ImplDX11_Shutdown();
		if (win32Ready) ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(imguiContext_);
		imguiContext_ = nullptr;
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

void RenderSession::LoadUiFonts() noexcept {
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
	const ImWchar* cjkRanges = extraGlyphRanges_.data();
	const float regularSize = std::round(kRegularFontSize * fontScale_);
	const float codeSize = std::round(kCodeFontSize * fontScale_);
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

bool RenderSession::RebuildUiFonts() noexcept {
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

void RenderSession::ScanMissingGlyphs(
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

bool RenderSession::GetOverlayBounds(
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

bool RenderSession::ShouldBuildFrame(UINT width, UINT height) noexcept {
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

void RenderSession::LogSourceOnce(const HookSpec& source) noexcept {
	const LONG bit = 1L << (static_cast<UINT32>(source.id) & 31);
	const LONG previous = InterlockedOr(&sourceLogMask_, bit);
	if ((previous & bit) == 0)
		DWM_LOG_FORMAT("Present source active: %s", source.name);
}

void RenderSession::ClearBackdrops() noexcept {
    for (auto& slot : backdrops_) {
        slot.compositor.Shutdown();
        slot.chain = slot.resource = slot.generation = 0;
    }
}

BackdropCompositor* RenderSession::SelectBackdrop(const FrameTarget& frame) noexcept {
    for (auto& slot : backdrops_) {
        if (frame.generationIdentity && slot.chain == frame.chainIdentity &&
            slot.generation != frame.generationIdentity) {
            // Buffer 0 was recreated: old copies no longer belong to this
            // chain generation, including same-size ResizeBuffers calls.
            slot.compositor.Shutdown();
            slot.chain = slot.resource = slot.generation = 0;
        }
    }
    for (auto& slot : backdrops_) {
        if (slot.chain == frame.chainIdentity && slot.resource == frame.resourceIdentity)
            return &slot.compositor;
    }
    for (auto& slot : backdrops_) {
        if (!slot.resource) {
            slot.chain = frame.chainIdentity;
            slot.resource = frame.resourceIdentity;
            slot.generation = frame.generationIdentity;
            return &slot.compositor;
        }
    }
    DWM_LOG_ONCE("Per-display buffer cache full; drawing skipped rather than mixing backgrounds");
    return nullptr;
}

bool RenderSession::Render(const FrameTarget& frame, const HookSpec& source,
    const UiSnapshot& snapshot, IAnswerProvider* answers, int scrollSteps, RECT& screenRect) noexcept {
    LogSourceOnce(source);
    ImGuiContext* previous = ImGui::GetCurrentContext();
    if (imguiContext_) ImGui::SetCurrentContext(imguiContext_);
    const bool result = RenderFrame(frame, snapshot, answers, scrollSteps, screenRect);
    ImGui::SetCurrentContext(previous);
    return result;
}

bool RenderSession::RenderFrame(const FrameTarget& frame, const UiSnapshot& snapshot,
    IAnswerProvider* answers, int scrollSteps, RECT& screenRect) noexcept {
    auto* backBuffer = frame.texture.Get();
    const auto& backBufferDescription = frame.description;
    if (GetTickCount64() >= nextDpiCheck_) {
        const float scale = frame.display.monitor
            ? ImGui_ImplWin32_GetDpiScaleForMonitor(frame.display.monitor)
            : ImGui_ImplWin32_GetDpiScaleForHwnd(GetDesktopWindow());
        if (scale > 0 && std::fabs(scale - fontScale_) > 0.01f) {
            fontScale_ = scale;
            if (imguiContext_) RebuildUiFonts();
        }
        nextDpiCheck_ = GetTickCount64() + 2000;
    }
    ComPtr<ID3D11Device> frameDevice;
    backBuffer->GetDevice(&frameDevice);
    if (!frameDevice || !EnsureDevice(frameDevice.Get())) return false;
    auto* compositor = SelectBackdrop(frame);
    if (!compositor) return false;
    ComPtr<ID3D11RenderTargetView> frameRenderTarget;
    if (FAILED(device_->CreateRenderTargetView(backBuffer, nullptr, &frameRenderTarget))) {
        DWM_LOG_ONCE("CreateRenderTargetView failed"); return false;
    }
    answerProvider_ = answers;

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

	if (scrollSteps || ShouldBuildFrame(backBufferDescription.Width,
		backBufferDescription.Height)) {
		ImGuiIO& io = ImGui::GetIO();
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
        inputSource_.SetDesktopOrigin({ frame.display.desktop.left, frame.display.desktop.top });
        inputSource_.Update(io);
		io.DisplaySize = ImVec2(
			static_cast<float>(backBufferDescription.Width),
			static_cast<float>(backBufferDescription.Height));
		ImGui::NewFrame();

		const UiFrameResult uiResult = ui_.Build(
			snapshot, answerProvider_, scrollSteps);
		ImGui::Render();
		hasCachedOverlayRect_ = GetOverlayBounds(
			uiResult.windowPosition, uiResult.windowSize,
			backBufferDescription.Width, backBufferDescription.Height,
			cachedOverlayRect_);
	}

    if (!hasCachedOverlayRect_) return false;
    if (frame.PrepareDraw && FAILED(frame.PrepareDraw(frame.operationContext, frame.plane,
        backBufferDescription.Width, backBufferDescription.Height))) {
        DWM_LOG_ONCE("Frame target dirty-rect preparation failed; drawing skipped");
        return false;
    }
    {
        D3D11OutputStateGuard stateGuard(context_.Get());
        if (!compositor->EnsureSize(device_.Get(), backBufferDescription)) return false;
        compositor->CaptureRegion(context_.Get(), backBuffer, cachedOverlayRect_);
        if (!compositor->RestoreBackdrop(context_.Get(), backBuffer, cachedOverlayRect_)) return false;
        ID3D11RenderTargetView* target = frameRenderTarget.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        compositor->SaveOutput(context_.Get(), backBuffer, cachedOverlayRect_);
    }
    if (frame.FinishDraw) frame.FinishDraw(context_.Get());
    screenRect = cachedOverlayRect_;
    OffsetRect(&screenRect, frame.display.desktop.left, frame.display.desktop.top);
    return true;
}

void RenderSession::ClearDeviceResources() noexcept {
	if (imguiContext_) {
        ImGui::SetCurrentContext(imguiContext_);
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(imguiContext_);
		imguiContext_ = nullptr;
	}
	ClearBackdrops();
	hasCachedOverlayRect_ = false;
	context_.Reset();
	device_.Reset();
}

void RenderSession::Shutdown() noexcept {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    const bool wasCurrent = previous == imguiContext_;
    ClearDeviceResources();
    ImGui::SetCurrentContext(wasCurrent ? nullptr : previous);
}

} // namespace dwm_overlay
