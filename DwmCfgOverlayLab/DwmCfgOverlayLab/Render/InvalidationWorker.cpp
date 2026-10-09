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
			const bool hasWork = regions_.Take(dirtyRect);
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

void InvalidationWorker::QueueTarget(UINT64 key, bool hasCurrentRect, const RECT& current) noexcept {
    AcquireSRWLockExclusive(&lock_);
    regions_.Track(key, hasCurrentRect, current);
    ReleaseSRWLockExclusive(&lock_);
    if (event_) SetEvent(event_);
}
void InvalidationWorker::EraseAll() noexcept {
    AcquireSRWLockExclusive(&lock_);
    regions_.EraseAll();
    ReleaseSRWLockExclusive(&lock_);
    if (event_) SetEvent(event_);
}
} // namespace dwm_overlay
