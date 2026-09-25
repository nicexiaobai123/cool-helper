#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>

#include "AnswerSink.h"
#include "AppTypes.h"
#include "OpenAIClient.h"
#include "OverlayControl.h"
#include "OverlayIpc.h"
#include "SettingsStore.h"

struct ImFont;

namespace coolhelper {

inline constexpr wchar_t kMainWindowClass[] = L"CoolHelperHub.MainWindow";

class App final {
public:
	App() = default;
	~App();

	int Run(HINSTANCE instance, int showCommand) noexcept;
	LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

private:
	static constexpr UINT kTrayMessage = WM_APP + 0x20;
	static constexpr UINT kHotkeyId = 1;
	static constexpr UINT kOverlayHotkeyId = 2;
	static constexpr UINT kOverlayScrollDownHotkeyId = 3;
	static constexpr UINT kOverlayScrollUpHotkeyId = 4;
	static constexpr UINT kTrayIconId = 1;

	bool CreateMainWindow(HINSTANCE instance) noexcept;
	bool CreateDevice() noexcept;
	void DestroyDevice() noexcept;
	void CreateRenderTarget() noexcept;
	void DestroyRenderTarget() noexcept;
	void InitializeImGui() noexcept;
	void ShutdownImGui() noexcept;
	void RenderFrame() noexcept;
	void RenderAnswerPage() noexcept;
	void RenderSettingsPage() noexcept;
	void RenderDwmPage() noexcept;
	void RefreshDwmStatus() noexcept;
	bool InjectOverlayDll() noexcept;
	bool EjectOverlayDll() noexcept;
	void ToggleOverlayVisible() noexcept;
	std::wstring ResolveOverlayDllPath() const noexcept;
	void ResetOverlayState() noexcept;
	void LoadSettings() noexcept;
	void CopySettingsToFields() noexcept;
	void SaveSettingsFromFields() noexcept;
	bool ApplyCaptureHotkey(
		const CaptureHotkeySettings& hotkey, std::string& error) noexcept;
	void UnregisterCaptureHotkey() noexcept;
	bool ApplyOverlayHotkey(
		const CaptureHotkeySettings& hotkey, std::string& error) noexcept;
	void UnregisterOverlayHotkey() noexcept;
	bool ApplyOverlayScrollHotkey(const CaptureHotkeySettings& hotkey,
		bool scrollDown, std::string& error) noexcept;
	void UnregisterOverlayScrollHotkeys() noexcept;
	void ScrollOverlayAnswer(int direction) noexcept;
	void ReloadHubFonts() noexcept;
	void ScanMissingAnswerGlyphs() noexcept;
	void TriggerCapture() noexcept;
	void DrainAnswerEvents() noexcept;
	void ShowMainWindow() noexcept;
	void HideMainWindow() noexcept;
	void AddTrayIcon() noexcept;
	void RemoveTrayIcon() noexcept;
	void ShowTrayMenu() noexcept;
	void RequestExit() noexcept;
	const char* StateText() const noexcept;

	HWND window_ = nullptr;
	HINSTANCE instance_ = nullptr;
	bool running_ = true;
	bool imguiReady_ = false;
	bool trayAdded_ = false;
	bool hotkeyRegistered_ = false;
	CaptureHotkeySettings registeredHotkey_;
	Microsoft::WRL::ComPtr<ID3D11Device> device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
	Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;

	SettingsStore settingsStore_;
	AppSettings settings_;
	QueuedAnswerSink answerSink_;
	OverlayIpcSink overlaySink_;
	OverlayControlChannel overlayControl_;
	TeeAnswerSink answerTee_;
	OpenAIClient aiClient_;
	RequestState requestState_ = RequestState::Idle;
	std::uint64_t nextRequestId_ = 1;
	std::uint64_t activeRequestId_ = 0;
	std::uint64_t lastSequence_ = 0;
	std::string answer_;
	std::string progressText_;
	std::string lastError_;
	ULONGLONG requestStartedTick_ = 0;
	std::string settingsStatus_;
	int screenshotWidth_ = 0;
	int screenshotHeight_ = 0;

	std::array<char, 1024> apiBaseUrlField_ = {};
	std::array<char, 1024> apiKeyField_ = {};
	std::array<char, 256> modelField_ = {};
	std::array<char, 8192> systemPromptField_ = {};
	std::array<char, 4096> userPromptField_ = {};
	std::array<char, 1024> overlayDllPathField_ = {};
	CaptureHotkeySettings captureHotkeyField_;
	CaptureHotkeySettings overlayHotkeyField_;
	CaptureHotkeySettings overlayScrollDownHotkeyField_;
	CaptureHotkeySettings overlayScrollUpHotkeyField_;
	CaptureHotkeySettings registeredOverlayHotkey_;
	bool overlayHotkeyRegistered_ = false;
	CaptureHotkeySettings registeredOverlayScrollDownHotkey_;
	CaptureHotkeySettings registeredOverlayScrollUpHotkey_;
	bool overlayScrollDownHotkeyRegistered_ = false;
	bool overlayScrollUpHotkeyRegistered_ = false;
	ImFont* fontRegular_ = nullptr;
	ImFont* fontTitle_ = nullptr;
	ImFont* fontCode_ = nullptr;
	// Characters discovered missing at runtime, accumulated as glyph ranges
	// and baked into the atlas on rebuild (dynamic glyph backfill).
	std::vector<std::uint16_t> extraGlyphRanges_;
	std::string pendingMissingGlyphs_;
	std::uint64_t glyphScannedUpTo_ = 0;
	ULONGLONG lastGlyphRebuildTick_ = 0;

	// DWM injection tab state; touched only on the UI thread.
	bool dwmBusy_ = false;
	bool dwmInjected_ = false;
	DWORD dwmProcessId_ = 0;
	ULONGLONG dwmLastRefreshTick_ = 0;
	std::string dwmStatus_;
	std::string dwmError_;
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

} // namespace coolhelper
