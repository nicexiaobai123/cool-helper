#include "AnswerIpc.h"

#include "../Common/Log.h"

namespace dwm_overlay {
namespace {

constexpr UINT64 kAnswerTextLimit = 512 * 1024;

UINT64 CopyTextInto(char* buffer, UINT64 capacity, const std::string& source) noexcept {
	if (!buffer || capacity == 0)
		return 0;
	const UINT64 count = source.size() < capacity - 1
		? static_cast<UINT64>(source.size())
		: capacity - 1;
	if (count > 0)
		memcpy(buffer, source.data(), static_cast<size_t>(count));
	buffer[count] = '\0';
	return count;
}

} // namespace

void AnswerIpcService::Start() noexcept {
	if (workerThread_)
		return;
	if (!stopEvent_) {
		stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!stopEvent_) {
			DWM_LOG("Answer IPC stop event could not be created");
			return;
		}
	}
	ResetEvent(stopEvent_);
	ResetModel();
	const HANDLE thread = CreateThread(nullptr, 0, WorkerProc, this, 0, nullptr);
	if (thread)
		workerThread_ = thread;
	else
		DWM_LOG("Answer IPC worker thread could not be created");
}

void AnswerIpcService::Stop() noexcept {
	if (stopEvent_) {
		SetEvent(stopEvent_);
	}
	if (workerThread_) {
		WaitForSingleObject(workerThread_, 5000);
		CloseHandle(workerThread_);
		workerThread_ = nullptr;
	}
	Detach("service stopped");
	if (stopEvent_) {
		CloseHandle(stopEvent_);
		stopEvent_ = nullptr;
	}
}

DWORD WINAPI AnswerIpcService::WorkerProc(void* context) noexcept {
	static_cast<AnswerIpcService*>(context)->WorkerLoop();
	return 0;
}

void AnswerIpcService::WorkerLoop() noexcept {
	for (;;) {
		if (!stopEvent_)
			break;
		const DWORD stopState = WaitForSingleObject(stopEvent_, 0);
		if (stopState == WAIT_OBJECT_0)
			break;

		if (!header_ && !TryAttach()) {
			const DWORD waited = WaitForSingleObject(stopEvent_, 500);
			if (waited == WAIT_OBJECT_0)
				break;
			continue;
		}

		Drain();

		if (HeartbeatFresh(header_->hubHeartbeatTick)) {
			header_->dllHeartbeatTick = GetTickCount64();
		}
		else {
			Detach("hub heartbeat lost");
			continue;
		}

		HANDLE waits[2] = { readyEvent_, stopEvent_ };
		const DWORD result = WaitForMultipleObjects(2, waits, FALSE, 500);
		if (result == WAIT_OBJECT_0 + 1)
			break;
		if (result == WAIT_OBJECT_0)
			continue; // Data arrived; drain at the top of the loop.
	}
}

bool AnswerIpcService::HeartbeatFresh(UINT64 tick) noexcept {
	const UINT64 now = GetTickCount64();
	return tick != 0 && now >= tick && now - tick < kIpcHeartbeatTimeoutMs;
}

bool AnswerIpcService::TryAttach() noexcept {
	const HANDLE mapping = OpenFileMappingW(
		FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kIpcSectionName);
	if (!mapping)
		return false;
	IpcSharedHeader* view = static_cast<IpcSharedHeader*>(MapViewOfFile(
		mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
	if (!view) {
		CloseHandle(mapping);
		return false;
	}
	if (view->protocolVersion != kIpcProtocolVersion ||
		view->headerSize != sizeof(IpcSharedHeader) ||
		view->slotCount != kIpcSlotCount ||
		view->slotSize != kIpcSlotSize) {
		DWM_LOG("Answer IPC version mismatch; waiting for a compatible hub");
		UnmapViewOfFile(view);
		CloseHandle(mapping);
		// Back off so a permanent mismatch cannot spin the worker.
		Sleep(2000);
		return false;
	}
	const HANDLE readyEvent = OpenEventW(SYNCHRONIZE, FALSE, kIpcReadyEventName);
	if (!readyEvent) {
		UnmapViewOfFile(view);
		CloseHandle(mapping);
		return false;
	}

	mapping_ = mapping;
	header_ = view;
	readyEvent_ = readyEvent;
	ResetModel();
	header_->dllHeartbeatTick = GetTickCount64();
	if (!attachedLogged_) {
		DWM_LOG("Answer IPC attached to CoolHelperHub");
		attachedLogged_ = true;
	}
	return true;
}

void AnswerIpcService::Detach(const char* reason) noexcept {
	if (header_) {
		DWM_LOG_FORMAT("Answer IPC detached: %s", reason ? reason : "unknown");
		attachedLogged_ = false;
	}
	if (readyEvent_) {
		CloseHandle(readyEvent_);
		readyEvent_ = nullptr;
	}
	if (header_) {
		UnmapViewOfFile(header_);
		header_ = nullptr;
	}
	if (mapping_) {
		CloseHandle(mapping_);
		mapping_ = nullptr;
	}
	ResetModel();
}

void AnswerIpcService::ResetModel() noexcept {
	AcquireSRWLockExclusive(&modelLock_);
	answerText_.clear();
	errorText_.clear();
	progressText_.clear();
	state_ = AnswerState::Idle;
	epoch_ = 0;
	lastSequence_ = 0;
	streamGap_ = false;
	truncated_ = false;
	ReleaseSRWLockExclusive(&modelLock_);
}

void AnswerIpcService::Drain() noexcept {
	IpcSharedHeader* header = header_;
	if (!header)
		return;
	const UINT64 write = header->writeIndex;
	UINT64 read = header->readIndex;
	if (read >= write)
		return;

	const UINT8* slots = reinterpret_cast<const UINT8*>(header) +
		header->slotDataOffset;
	while (read < write) {
		const UINT8* slot = slots + (read & (kIpcSlotCount - 1)) * kIpcSlotSize;
		const auto messageHeader = reinterpret_cast<const IpcMessageHeader*>(slot);
		if (messageHeader->payloadSize <= kIpcMaxPayload) {
			ApplyMessage(*messageHeader,
				reinterpret_cast<const char*>(slot + kIpcMessageHeaderSize));
		}
		MemoryBarrier();
		header->readIndex = ++read;
	}
}

void AnswerIpcService::ApplyMessage(
	const IpcMessageHeader& header, const char* payload) noexcept {
	const UINT64 epoch = header.epoch;
	const UINT64 sequence = header.sequence;
	const auto type = static_cast<IpcMessageType>(header.type);
	const UINT64 payloadSize = header.payloadSize;
	const bool continuation =
		(header.flags & kIpcMessageFlagContinuation) != 0;

	AcquireSRWLockExclusive(&modelLock_);
	switch (type) {
	case IpcMessageType::Started:
		answerText_.clear();
		errorText_.clear();
		progressText_.clear();
		state_ = AnswerState::Connecting;
		epoch_ = epoch;
		lastSequence_ = sequence;
		streamGap_ = false;
		truncated_ = false;
		break;
	case IpcMessageType::Progress:
		if (epoch_ != epoch) {
			answerText_.clear();
			errorText_.clear();
			epoch_ = epoch;
			lastSequence_ = 0;
			streamGap_ = false;
			truncated_ = false;
		}
		progressText_.assign(payload, static_cast<size_t>(payloadSize));
		state_ = progressText_.empty()
			? AnswerState::Waiting : AnswerState::Thinking;
		break;
	case IpcMessageType::Delta:
		if (continuation) {
			// Split-payload tail of the previous sequence; append directly.
			if (epoch_ != epoch) {
				ReleaseSRWLockExclusive(&modelLock_);
				return;
			}
		}
		else if (epoch_ != epoch) {
			// A request the attach/detach boundary skipped the Started for.
			answerText_.clear();
			errorText_.clear();
			epoch_ = epoch;
			lastSequence_ = sequence;
			state_ = AnswerState::Streaming;
			streamGap_ = false;
			truncated_ = false;
		}
		else if (sequence == lastSequence_ + 1) {
			lastSequence_ = sequence;
		}
		else if (sequence <= lastSequence_) {
			ReleaseSRWLockExclusive(&modelLock_);
			return;
		}
		else {
			// Reserve one slot for the gap marker below; with the cap reached
			// we keep the already-received head instead of growing further.
			if (answerText_.size() + payloadSize + 16 <= kAnswerTextLimit) {
				answerText_.append("\n\u2026[\u4e22\u5931\u4e00\u6bb5\u5185\u5bb9]\u2026\n");
				streamGap_ = true;
			}
			lastSequence_ = sequence;
		}
		if (answerText_.size() + payloadSize + 1 <= kAnswerTextLimit) {
			answerText_.append(payload, static_cast<size_t>(payloadSize));
		}
		else {
			truncated_ = true;
		}
		progressText_.clear();
		state_ = AnswerState::Streaming;
		break;
	case IpcMessageType::Completed:
		progressText_.clear();
		state_ = AnswerState::Completed;
		break;
	case IpcMessageType::Failed:
		if (epoch_ != epoch) {
			answerText_.clear();
			epoch_ = epoch;
		}
		errorText_.assign(payload, static_cast<size_t>(payloadSize));
		progressText_.clear();
		state_ = AnswerState::Failed;
		break;
	case IpcMessageType::Cancelled:
		progressText_.clear();
		state_ = AnswerState::Cancelled;
		break;
	case IpcMessageType::Cleared:
		answerText_.clear();
		errorText_.clear();
		progressText_.clear();
		state_ = AnswerState::Idle;
		epoch_ = 0;
		lastSequence_ = 0;
		streamGap_ = false;
		truncated_ = false;
		break;
	default:
		break;
	}
	ReleaseSRWLockExclusive(&modelLock_);
}

bool AnswerIpcService::IsHubConnected() const noexcept {
	const IpcSharedHeader* header = header_;
	if (!header)
		return false;
	return HeartbeatFresh(header->hubHeartbeatTick);
}

AnswerState AnswerIpcService::GetAnswerState() const noexcept {
	AcquireSRWLockShared(&modelLock_);
	const AnswerState state = state_;
	ReleaseSRWLockShared(&modelLock_);
	return state;
}

UINT64 AnswerIpcService::GetAnswerEpoch() const noexcept {
	AcquireSRWLockShared(&modelLock_);
	const UINT64 epoch = epoch_;
	ReleaseSRWLockShared(&modelLock_);
	return epoch;
}

UINT64 AnswerIpcService::CopyAnswerText(
	char* buffer, UINT64 capacity) const noexcept {
	AcquireSRWLockShared(&modelLock_);
	const UINT64 copied = CopyTextInto(buffer, capacity, answerText_);
	ReleaseSRWLockShared(&modelLock_);
	return copied;
}

UINT64 AnswerIpcService::CopyErrorText(
	char* buffer, UINT64 capacity) const noexcept {
	AcquireSRWLockShared(&modelLock_);
	const UINT64 copied = CopyTextInto(buffer, capacity, errorText_);
	ReleaseSRWLockShared(&modelLock_);
	return copied;
}

UINT64 AnswerIpcService::CopyProgressText(
	char* buffer, UINT64 capacity) const noexcept {
	AcquireSRWLockShared(&modelLock_);
	const UINT64 copied = CopyTextInto(buffer, capacity, progressText_);
	ReleaseSRWLockShared(&modelLock_);
	return copied;
}

bool AnswerIpcService::HasStreamGap() const noexcept {
	AcquireSRWLockShared(&modelLock_);
	const bool gap = streamGap_;
	ReleaseSRWLockShared(&modelLock_);
	return gap;
}

bool AnswerIpcService::IsTruncated() const noexcept {
	AcquireSRWLockShared(&modelLock_);
	const bool truncated = truncated_;
	ReleaseSRWLockShared(&modelLock_);
	return truncated;
}

} // namespace dwm_overlay
