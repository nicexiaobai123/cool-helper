#include "InvalidationWorker.h"
#include "../Common/Log.h"

namespace dwm_overlay {

BOOL CALLBACK InvalidationWorker::InvalidateIntersectingWindow(
	HWND window,
	LPARAM parameter) noexcept {
	if (!IsWindowVisible(window) || IsIconic(window))
		return TRUE;
	const RECT& screenDirtyRect = *reinterpret_cast<const RECT*>(parameter);
	RECT windowRect = {};
	RECT intersection = {};
	if (!GetWindowRect(window, &windowRect) ||
		!IntersectRect(&intersection, &screenDirtyRect, &windowRect))
		return TRUE;

	POINT localPoints[2] = {
		{ intersection.left, intersection.top },
		{ intersection.right, intersection.bottom }
	};
	MapWindowPoints(HWND_DESKTOP, window, localPoints, 2);
	const RECT localDirtyRect = {
		localPoints[0].x, localPoints[0].y,
		localPoints[1].x, localPoints[1].y
	};
	RedrawWindow(window, &localDirtyRect, nullptr,
		RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_NOERASE);
	return TRUE;
}

DWORD WINAPI InvalidationWorker::ThreadEntry(void* parameter) noexcept {
	return static_cast<InvalidationWorker*>(parameter)->Run();
}

DWORD InvalidationWorker::Run() noexcept {
	for (;;) {
		if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0)
			return 0;
		for (;;) {
			RECT dirtyRect = {};
			AcquireSRWLockExclusive(&lock_);
			const bool hasWork = hasPendingRect_;
			if (hasWork) {
				dirtyRect = pendingRect_;
				hasPendingRect_ = false;
			}
			ReleaseSRWLockExclusive(&lock_);
			if (!hasWork)
				break;
			EnumWindows(InvalidateIntersectingWindow,
				reinterpret_cast<LPARAM>(&dirtyRect));
			RedrawWindow(GetDesktopWindow(), &dirtyRect, nullptr,
				RDW_INVALIDATE | RDW_NOERASE);
			DWM_LOG_ONCE("Moved overlay region invalidated asynchronously");
		}
		if (InterlockedCompareExchange(&stopping_, 0, 0) != 0)
			return 0;
	}
}

bool InvalidationWorker::Start() noexcept {
	if (event_)
		return true;
	InterlockedExchange(&stopping_, 0);
	event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!event_) {
		DWM_LOG("Unable to create overlay invalidation event");
		return false;
	}
	thread_ = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
	if (!thread_) {
		CloseHandle(event_);
		event_ = nullptr;
		DWM_LOG("Unable to create overlay invalidation worker");
		return false;
	}
	DWM_LOG("Handleless overlay invalidation worker ready");
	return true;
}

void InvalidationWorker::Stop(DWORD timeoutMs) noexcept {
	if (!event_)
		return;
	InterlockedExchange(&stopping_, 1);
	SetEvent(event_);
	if (thread_) {
		const DWORD wait = WaitForSingleObject(thread_, timeoutMs);
		if (wait == WAIT_OBJECT_0) {
			CloseHandle(thread_);
			thread_ = nullptr;
			CloseHandle(event_);
			event_ = nullptr;
			DWM_LOG("Overlay invalidation worker stopped");
		}
		else {
			DWM_LOG("Overlay invalidation worker stop timed out");
		}
	}
}

RECT InvalidationWorker::ToScreen(
	IDXGISwapChain* swapChain,
	const RECT& swapChainRect) noexcept {
	RECT screenRect = swapChainRect;
	IDXGISwapChain* standardSwapChain = nullptr;
	if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&standardSwapChain))))
		return screenRect;
	IDXGIOutput* output = nullptr;
	if (SUCCEEDED(standardSwapChain->GetContainingOutput(&output))) {
		DXGI_OUTPUT_DESC description = {};
		if (SUCCEEDED(output->GetDesc(&description))) {
			OffsetRect(&screenRect, description.DesktopCoordinates.left,
				description.DesktopCoordinates.top);
		}
		output->Release();
	}
	standardSwapChain->Release();
	return screenRect;
}

void InvalidationWorker::QueueMovedOverlay(
	IDXGISwapChain* swapChain,
	bool hasCurrentRect,
	const RECT& currentSwapChainRect) noexcept {
	RECT currentScreenRect = {};
	if (hasCurrentRect) {
		currentScreenRect = ToScreen(swapChain, currentSwapChainRect);
	}
	else if (!hasLastScreenRect_) {
		return; // Nothing was drawn before; no region to recompose.
	}

	// Queue the union of the previous and current regions on every presented
	// frame: the translucent UI blends over the composed desktop, so the
	// compositor must redraw this region constantly. That keeps the backdrop
	// live and leaves no pixels of this UI or of windows passing over the
	// overlay area.
	const bool hadPreviousRect = hasLastScreenRect_;
	RECT dirtyRect = {};
	bool hasDirtyRect = false;
	if (hadPreviousRect) {
		dirtyRect = lastScreenRect_;
		hasDirtyRect = true;
	}
	if (hasCurrentRect) {
		if (hasDirtyRect)
			UnionRect(&dirtyRect, &dirtyRect, &currentScreenRect);
		else {
			dirtyRect = currentScreenRect;
			hasDirtyRect = true;
		}
	}

	lastSwapChain_ = swapChain;
	lastScreenRect_ = currentScreenRect;
	hasLastScreenRect_ = hasCurrentRect;
	if (!hasDirtyRect || !event_)
		return;

	AcquireSRWLockExclusive(&lock_);
	if (hasPendingRect_)
		UnionRect(&pendingRect_, &pendingRect_, &dirtyRect);
	else {
		pendingRect_ = dirtyRect;
		hasPendingRect_ = true;
	}
	ReleaseSRWLockExclusive(&lock_);
	SetEvent(event_);
}

} // namespace dwm_overlay
