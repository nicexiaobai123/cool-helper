#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

// One wire contract for the hub and DLL. v1 keeps its original 72-byte header;
// display configuration uses previously reserved words, so old peers still
// support visibility/scroll and simply report no display-policy capability.
namespace coolhelper_overlay {

enum class DisplayMode : UINT32 { Compatible = 0, PrimaryOnly = 1, AllDisplays = 2 };
constexpr bool IsDisplayMode(UINT32 value) noexcept { return value <= 2; }
constexpr UINT32 kDisplayPolicyCapability = 0x80000000u;

constexpr UINT32 kControlProtocolVersion = 1;
constexpr UINT32 kControlSlotCount = 16;
constexpr UINT32 kControlSlotSize = 64;
constexpr UINT64 kControlHeartbeatTimeoutMs = 8000;
#if defined(COOLHELPER_CONTROL_TEST_NAMESPACE)
inline constexpr wchar_t kControlSectionName[] = L"Local\\CoolHelper.Tests.Overlay.Control.v1";
inline constexpr wchar_t kControlReadyEventName[] = L"Local\\CoolHelper.Tests.Overlay.Control.Ready.v1";
#else
inline constexpr wchar_t kControlSectionName[] = L"Local\\CoolHelper.Overlay.Control.v1";
inline constexpr wchar_t kControlReadyEventName[] = L"Local\\CoolHelper.Overlay.Control.Ready.v1";
#endif
constexpr UINT32 kControlCommandSetOverlayVisible = 1;
constexpr UINT32 kControlCommandScrollAnswer = 2;
constexpr UINT32 kControlCommandSetDisplayMode = 3;
constexpr UINT32 kControlScrollUp = 0;
constexpr UINT32 kControlScrollDown = 1;

#pragma pack(push, 8)
struct ControlSharedHeader {
    UINT32 protocolVersion;
    UINT32 headerSize;
    UINT32 slotCount;
    UINT32 slotSize;
    UINT32 slotDataOffset;
    volatile UINT32 hubDisplayMode; // persisted desired mode, even before DLL attaches
    volatile UINT64 hubHeartbeatTick;
    volatile UINT64 dllHeartbeatTick;
    volatile UINT64 writeIndex;
    volatile UINT64 readIndex;
    volatile UINT64 droppedByWriter;
    volatile UINT32 dllOverlayVisible;
    volatile UINT32 dllDisplayState; // capability bit | applied requested mode
};
struct ControlCommandMessage {
    UINT32 type;
    UINT32 value;
    UINT32 sequence;
    UINT32 reserved;
};
#pragma pack(pop)
static_assert(sizeof(ControlSharedHeader) == 72, "v1 header ABI");
static_assert(offsetof(ControlSharedHeader, hubDisplayMode) == 20, "v1 reserved word");
static_assert(offsetof(ControlSharedHeader, dllDisplayState) == 68, "v1 reserved word");
static_assert(sizeof(ControlCommandMessage) <= kControlSlotSize, "command slot");
static_assert((kControlSlotCount & (kControlSlotCount - 1)) == 0, "power of two");
} // namespace coolhelper_overlay
