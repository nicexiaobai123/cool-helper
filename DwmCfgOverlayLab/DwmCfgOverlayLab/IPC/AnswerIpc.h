#pragma once

#include <Windows.h>

#include <string>

#include "UiState.h"

namespace dwm_overlay {

// Shared transport contract with CoolHelperHub's OverlayIpc sink. The two
// projects do not share headers, so every constant, struct, and field order
// here must stay byte-identical with CoolHelperHub's src/OverlayIpc.cpp.
constexpr UINT32 kIpcProtocolVersion = 1;
constexpr UINT32 kIpcSlotCount = 128; // power of two; slot selection uses a mask
constexpr UINT32 kIpcSlotSize = 64 * 1024;
constexpr UINT32 kIpcMessageHeaderSize = 24;
constexpr UINT32 kIpcMaxPayload = kIpcSlotSize - kIpcMessageHeaderSize;
constexpr UINT32 kIpcMessageFlagContinuation = 0x1;
constexpr UINT64 kIpcHeartbeatTimeoutMs = 8000;
constexpr wchar_t kIpcSectionName[] = L"Local\\CoolHelper.Overlay.Answer.v1";
constexpr wchar_t kIpcReadyEventName[] = L"Local\\CoolHelper.Overlay.Answer.Ready.v1";

enum class IpcMessageType : UINT32 {
	Started = 0,
	Delta = 1,
	Completed = 2,
	Failed = 3,
	Cancelled = 4,
	Cleared = 5
};

#pragma pack(push, 8)
struct IpcSharedHeader {
	UINT32 protocolVersion;
	UINT32 headerSize;
	UINT32 slotCount;
	UINT32 slotSize;
	UINT32 slotDataOffset;
	UINT32 reserved0;
	volatile UINT64 hubHeartbeatTick;
	volatile UINT64 dllHeartbeatTick;
	volatile UINT64 writeIndex;
	volatile UINT64 readIndex;
	volatile UINT64 droppedByWriter;
};

struct IpcMessageHeader {
	UINT32 payloadSize;
	UINT32 type;    // IpcMessageType
	UINT32 epoch;   // hub request id (low 32 bits)
	UINT32 sequence;
	UINT32 flags;   // kIpcMessageFlagContinuation for split-payload chunks
	UINT32 reserved;
};
#pragma pack(pop)

static_assert(sizeof(IpcSharedHeader) == 64, "layout must match CoolHelperHub");
static_assert(sizeof(IpcMessageHeader) == kIpcMessageHeaderSize,
	"layout must match CoolHelperHub");static_assert((kIpcSlotCount & (kIpcSlotCount - 1)) == 0,
	"slot count must be a power of two");

// Consumes the hub's answer stream without ever blocking the compositor: the
// worker thread owns all waits, and the Present thread only takes an SRWLock
// to copy the latest text.
class AnswerIpcService final : public IAnswerProvider {
public:
	AnswerIpcService() = default;
	~AnswerIpcService() = default;
	AnswerIpcService(const AnswerIpcService&) = delete;
	AnswerIpcService& operator=(const AnswerIpcService&) = delete;

	void Start() noexcept;
	void Stop() noexcept;

	// IAnswerProvider (called on the Present thread).
	bool IsHubConnected() const noexcept override;
	AnswerState GetAnswerState() const noexcept override;
	UINT64 CopyAnswerText(char* buffer, UINT64 capacity) const noexcept override;
	UINT64 CopyErrorText(char* buffer, UINT64 capacity) const noexcept override;
	bool HasStreamGap() const noexcept override;
	bool IsTruncated() const noexcept override;

private:
	static DWORD WINAPI WorkerProc(void* context) noexcept;
	void WorkerLoop() noexcept;
	bool TryAttach() noexcept;
	void Detach(const char* reason) noexcept;
	void Drain() noexcept;
	void ApplyMessage(const IpcMessageHeader& header, const char* payload) noexcept;
	void ResetModel() noexcept;
	static bool HeartbeatFresh(UINT64 tick) noexcept;

	HANDLE workerThread_ = nullptr;
	HANDLE stopEvent_ = nullptr;
	HANDLE mapping_ = nullptr;
	HANDLE readyEvent_ = nullptr;
	IpcSharedHeader* header_ = nullptr;

	mutable SRWLOCK modelLock_ = SRWLOCK_INIT;
	std::string answerText_;
	std::string errorText_;
	AnswerState state_ = AnswerState::Idle;
	UINT64 epoch_ = 0;
	UINT64 lastSequence_ = 0;
	bool streamGap_ = false;
	bool truncated_ = false;
	bool attachedLogged_ = false;
};

} // namespace dwm_overlay
