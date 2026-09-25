#pragma once

#include <Windows.h>

namespace dwm_overlay {

struct UiSnapshot {
	UINT64 revision = 0;
	bool ipcConnected = false;
	char status[160] = {};
};

enum class AnswerState : UINT32 {
	Idle,
	Connecting,
	Waiting,
	Thinking,
	Streaming,
	Completed,
	Failed,
	Cancelled
};

// Read-only view of the answer text received over IPC. Implemented by the IPC
// service; OverlayUi consumes it on the Present thread.
class IAnswerProvider {
public:
	virtual ~IAnswerProvider() = default;
	virtual bool IsHubConnected() const noexcept = 0;
	virtual AnswerState GetAnswerState() const noexcept = 0;
	virtual UINT64 CopyAnswerText(char* buffer, UINT64 capacity) const noexcept = 0;
	virtual UINT64 CopyErrorText(char* buffer, UINT64 capacity) const noexcept = 0;
	virtual UINT64 CopyProgressText(char* buffer, UINT64 capacity) const noexcept = 0;

	virtual bool HasStreamGap() const noexcept = 0;
	virtual bool IsTruncated() const noexcept = 0;
};

// Three buffers keep Present-side reads wait-free. The IPC worker never
// overwrites the slot currently being copied by the compositor thread.
class UiStateStore final {
public:
	UiSnapshot Read() const noexcept;
	void Publish(const UiSnapshot& snapshot) noexcept;

private:
	mutable SRWLOCK writerLock_ = SRWLOCK_INIT;
	UiSnapshot snapshots_[3] = {};
	mutable volatile LONG publishedIndex_ = 0;
	mutable volatile LONG readerIndex_ = -1;
};

} // namespace dwm_overlay
