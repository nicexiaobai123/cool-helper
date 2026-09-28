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

struct AnswerSnapshot {
	bool hubConnected = false;
	AnswerState state = AnswerState::Idle;
	UINT64 epoch = 0;
	UINT64 answerLength = 0;
	UINT64 errorLength = 0;
	UINT64 progressLength = 0;
	bool streamGap = false;
	bool truncated = false;
};

// Read-only view of the answer text received over IPC. Implemented by the IPC
// service; OverlayUi consumes it on the Present thread.
class IAnswerProvider {
public:
	virtual ~IAnswerProvider() = default;
	virtual AnswerSnapshot CopySnapshot(
		char* answerBuffer, UINT64 answerCapacity,
		char* errorBuffer, UINT64 errorCapacity,
		char* progressBuffer, UINT64 progressCapacity) const noexcept = 0;
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
