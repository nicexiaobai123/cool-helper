#pragma once

#include <Windows.h>
#include <dxgi.h>

namespace dwm_overlay {

class InvalidationWorker final {
public:
	bool Start() noexcept;
	void Stop(DWORD timeoutMs = 2000) noexcept;
	void QueueMovedOverlay(
		IDXGISwapChain* swapChain,
		bool hasCurrentRect,
		const RECT& currentSwapChainRect) noexcept;

private:
	static DWORD WINAPI ThreadEntry(void* parameter) noexcept;
	static BOOL CALLBACK InvalidateIntersectingWindow(
		HWND window,
		LPARAM parameter) noexcept;
	DWORD Run() noexcept;
	static RECT ToScreen(
		IDXGISwapChain* swapChain,
		const RECT& swapChainRect) noexcept;

	SRWLOCK lock_ = SRWLOCK_INIT;
	HANDLE event_ = nullptr;
	HANDLE thread_ = nullptr;
	volatile LONG stopping_ = 0;
	RECT pendingRect_ = {};
	bool hasPendingRect_ = false;
	IDXGISwapChain* lastSwapChain_ = nullptr;
	RECT lastScreenRect_ = {};
	bool hasLastScreenRect_ = false;
};

} // namespace dwm_overlay
