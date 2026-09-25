#pragma once

#include <Windows.h>

namespace dwm_overlay {

// Reverse control channel: CoolHelperHub writes low-frequency commands (hide
// / show the overlay window, more types may follow) into a small ring buffer;
// this service consumes them on its own worker thread. The transport contract
// is mirrored in CoolHelperHub's include/coolhelper/OverlayControl.h and must
// stay byte-identical.
constexpr UINT32 kControlProtocolVersion = 1;
constexpr UINT32 kControlSlotCount = 16; // power of two; slot selection uses a mask
constexpr UINT32 kControlSlotSize = 64;
constexpr UINT64 kControlHeartbeatTimeoutMs = 8000;
constexpr wchar_t kControlSectionName[] = L"Local\\CoolHelper.Overlay.Control.v1";
constexpr wchar_t kControlReadyEventName[] = L"Local\\CoolHelper.Overlay.Control.Ready.v1";

constexpr UINT32 kControlCommandSetOverlayVisible = 1; // value: 1=show, 0=hide

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

static_assert(sizeof(ControlSharedHeader) == 72, "layout must match CoolHelperHub");
static_assert(sizeof(ControlCommandMessage) <= kControlSlotSize,
	"command must fit a slot");
static_assert((kControlSlotCount & (kControlSlotCount - 1)) == 0,
	"slot count must be a power of two");

// Consumes hub control commands. The callbacks are invoked on the worker
// thread and must only touch state that the compositor reads atomically.
class ControlIpcService final {
public:
	struct Callbacks {
		void (*OnCommand)(void* context, UINT32 type, UINT32 value) noexcept;
		bool (*QueryOverlayVisible)(void* context) noexcept;
		void* context;
	};

	ControlIpcService(const Callbacks& callbacks) noexcept;
	~ControlIpcService() = default;
	ControlIpcService(const ControlIpcService&) = delete;
	ControlIpcService& operator=(const ControlIpcService&) = delete;

	void Start() noexcept;
	void Stop() noexcept;

private:
	static DWORD WINAPI WorkerProc(void* context) noexcept;
	void WorkerLoop() noexcept;
	bool TryAttach() noexcept;
	void Detach(const char* reason) noexcept;
	void Drain() noexcept;
	static bool HeartbeatFresh(UINT64 tick) noexcept;

	Callbacks callbacks_ = {};
	HANDLE workerThread_ = nullptr;
	HANDLE stopEvent_ = nullptr;
	HANDLE mapping_ = nullptr;
	HANDLE readyEvent_ = nullptr;
	ControlSharedHeader* header_ = nullptr;
	bool attachedLogged_ = false;
};

} // namespace dwm_overlay
