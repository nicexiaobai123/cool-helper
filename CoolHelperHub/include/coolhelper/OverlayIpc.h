#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "AnswerSink.h"

namespace coolhelper {

// Producer side of the shared-memory answer stream consumed by
// DwmCfgOverlayLab inside dwm.exe. The transport contract is duplicated in
// DwmCfgOverlayLab's IPC/AnswerIpc.h; keep both copies byte-identical.
namespace overlayipc {

constexpr UINT32 kIpcProtocolVersion = 1;
constexpr UINT32 kIpcSlotCount = 128; // power of two; slot selection uses a mask
constexpr UINT32 kIpcSlotSize = 64 * 1024;
constexpr UINT32 kIpcMessageHeaderSize = 24;
constexpr UINT32 kIpcMaxPayload = kIpcSlotSize - kIpcMessageHeaderSize;
constexpr UINT32 kIpcMessageFlagContinuation = 0x1;
constexpr UINT64 kIpcHeartbeatTimeoutMs = 8000;
inline constexpr wchar_t kIpcSectionName[] = L"Local\\CoolHelper.Overlay.Answer.v1";
inline constexpr wchar_t kIpcReadyEventName[] = L"Local\\CoolHelper.Overlay.Answer.Ready.v1";

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
	UINT32 type;
	UINT32 epoch;
	UINT32 sequence;
	UINT32 flags; // kIpcMessageFlagContinuation for split-payload chunks
	UINT32 reserved;
};
#pragma pack(pop)

static_assert(sizeof(IpcSharedHeader) == 64, "layout must match DwmCfgOverlayLab");
static_assert(sizeof(IpcMessageHeader) == kIpcMessageHeaderSize,
	"layout must match DwmCfgOverlayLab");

} // namespace overlayipc

// Non-blocking IAnswerSink that forwards ordered AnswerEvents to the overlay
// DLL. Publish may be called from the network thread; a dedicated sender
// thread owns every wait on the shared ring buffer.
class OverlayIpcSink final : public IAnswerSink {
public:
	OverlayIpcSink() = default;
	~OverlayIpcSink() override;

	OverlayIpcSink(const OverlayIpcSink&) = delete;
	OverlayIpcSink& operator=(const OverlayIpcSink&) = delete;

	bool Start(std::string& error) noexcept;
	void Stop() noexcept;
	bool IsRunning() const noexcept { return running_.load(); }

	// True while DwmCfgOverlayLab's worker heartbeats the shared header.
	bool IsConnected() const noexcept;
	const char* DescribeStatus() const noexcept;

	// Only forwards events whose requestId matches the active request, so a
	// cancelled request's trailing deltas cannot pollute the overlay text.
	void SetRequestIdFilter(std::uint64_t requestId) noexcept;

	void Publish(const AnswerEvent& event) noexcept override;

private:
	static DWORD WINAPI SenderProc(void* context) noexcept;
	void SenderLoop() noexcept;
	bool WriteMessage(UINT32 type, UINT32 epoch, UINT32 sequence,
		std::string_view payload) noexcept;
	bool WriteMessage(UINT32 type, UINT32 epoch, UINT32 sequence,
		std::string_view payload, UINT32 flags) noexcept;
	static bool HeartbeatFresh(UINT64 tick) noexcept;

	std::atomic_bool running_ = false;
	HANDLE senderThread_ = nullptr;
	HANDLE stopEvent_ = nullptr;   // manual reset, unnamed
	HANDLE notifyEvent_ = nullptr; // auto reset, unnamed
	HANDLE readyEvent_ = nullptr;  // auto reset, named, shared with the DLL
	HANDLE mapping_ = nullptr;
	overlayipc::IpcSharedHeader* header_ = nullptr;

	std::mutex queueMutex_;
	std::deque<AnswerEvent> queue_;
	bool queueDropped_ = false;
	std::atomic<std::uint64_t> requestIdFilter_ = 0; // 0 accepts everything
};

} // namespace coolhelper
