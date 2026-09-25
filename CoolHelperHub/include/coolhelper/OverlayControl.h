#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace coolhelper {

// Writer side of the reverse control channel consumed by DwmCfgOverlayLab's
// ControlIpcService. The transport contract is mirrored in
// DwmCfgOverlayLab's IPC/ControlIpc.h and must stay byte-identical.
namespace overlaycontrol {

constexpr UINT32 kControlProtocolVersion = 1;
constexpr UINT32 kControlSlotCount = 16; // power of two
constexpr UINT32 kControlSlotSize = 64;
constexpr UINT64 kControlHeartbeatTimeoutMs = 8000;
inline constexpr wchar_t kControlSectionName[] = L"Local\\CoolHelper.Overlay.Control.v1";
inline constexpr wchar_t kControlReadyEventName[] = L"Local\\CoolHelper.Overlay.Control.Ready.v1";

constexpr UINT32 kControlCommandSetOverlayVisible = 1; // value: 1=show, 0=hide
constexpr UINT32 kControlCommandScrollAnswer = 2;
constexpr UINT32 kControlScrollUp = 0;
constexpr UINT32 kControlScrollDown = 1;

#pragma pack(push, 8)
struct ControlSharedHeader {
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
	volatile UINT32 dllOverlayVisible; // DLL-reported current state (1/0)
	volatile UINT32 reserved1;
};

struct ControlCommandMessage {
	UINT32 type;
	UINT32 value;
	UINT32 sequence;
	UINT32 reserved;
};
#pragma pack(pop)

static_assert(sizeof(ControlSharedHeader) == 72, "layout must match DwmCfgOverlayLab");
static_assert(sizeof(ControlCommandMessage) <= kControlSlotSize,
	"command must fit a slot");

} // namespace overlaycontrol

// Low-frequency command writer. Commands are sent synchronously from the UI
// thread (a single writer by design); liveness is driven by KeepAlive() which
// the app calls once per message-loop iteration.
class OverlayControlChannel final {
public:
	OverlayControlChannel() = default;
	~OverlayControlChannel();
	OverlayControlChannel(const OverlayControlChannel&) = delete;
	OverlayControlChannel& operator=(const OverlayControlChannel&) = delete;

	bool Start(std::string& error) noexcept;
	void Stop() noexcept;
	bool IsRunning() const noexcept { return running_; }

	// Called once per app loop iteration to refresh the hub heartbeat.
	void KeepAlive() noexcept;

	// True while DwmCfgOverlayLab's control worker heartbeats the header.
	bool IsDllConnected() const noexcept;

	// -1 = unknown, 0 = hidden, 1 = visible (DLL-reported).
	int QueryOverlayVisible() const noexcept;

	bool SendSetOverlayVisible(bool visible) noexcept;
	bool SendScrollAnswer(int direction) noexcept;

private:
	static bool HeartbeatFresh(UINT64 tick) noexcept;
	bool SendCommand(UINT32 type, UINT32 value) noexcept;

	bool running_ = false;
	HANDLE readyEvent_ = nullptr;
	HANDLE mapping_ = nullptr;
	overlaycontrol::ControlSharedHeader* header_ = nullptr;
	UINT32 sequence_ = 0;
};

} // namespace coolhelper
