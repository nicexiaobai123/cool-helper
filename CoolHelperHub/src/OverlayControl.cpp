#include "coolhelper/OverlayControl.h"

#include <cstring>

namespace coolhelper {
namespace {

using overlaycontrol::ControlSharedHeader;

constexpr UINT64 kControlSectionSize = sizeof(ControlSharedHeader) +
	overlaycontrol::kControlSlotCount * overlaycontrol::kControlSlotSize;

bool MakeOpenSecurityAttributes(SECURITY_ATTRIBUTES* attributes) noexcept {
	attributes->nLength = sizeof(*attributes);
	attributes->bInheritHandle = FALSE;
	auto* descriptor = new (std::nothrow) SECURITY_DESCRIPTOR();
	if (!descriptor)
		return false;
	if (!InitializeSecurityDescriptor(descriptor, SECURITY_DESCRIPTOR_REVISION) ||
		!SetSecurityDescriptorDacl(descriptor, TRUE, nullptr, FALSE)) {
		delete descriptor;
		return false;
	}
	attributes->lpSecurityDescriptor = descriptor;
	return true;
}

void FreeSecurityDescriptor(SECURITY_ATTRIBUTES* attributes) noexcept {
	delete static_cast<SECURITY_DESCRIPTOR*>(attributes->lpSecurityDescriptor);
	attributes->lpSecurityDescriptor = nullptr;
}

} // namespace

OverlayControlChannel::~OverlayControlChannel() {
	Stop();
}

bool OverlayControlChannel::Start(std::string& error) noexcept {
	if (running_)
		return true;

	SECURITY_ATTRIBUTES security = {};
	if (!MakeOpenSecurityAttributes(&security)) {
		error = "无法创建控制通道安全描述符";
		return false;
	}
	const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security,
		PAGE_READWRITE,
		static_cast<DWORD>(kControlSectionSize >> 32),
		static_cast<DWORD>(kControlSectionSize & 0xFFFFFFFFu),
		overlaycontrol::kControlSectionName);
	FreeSecurityDescriptor(&security);
	if (!mapping) {
		error = "无法创建控制通道共享内存：" + std::to_string(GetLastError());
		return false;
	}
	auto* header = static_cast<ControlSharedHeader*>(MapViewOfFile(
		mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
	if (!header) {
		CloseHandle(mapping);
		error = "无法映射控制通道共享内存：" + std::to_string(GetLastError());
		return false;
	}

	SECURITY_ATTRIBUTES eventSecurity = {};
	if (!MakeOpenSecurityAttributes(&eventSecurity)) {
		UnmapViewOfFile(header);
		CloseHandle(mapping);
		error = "无法创建控制通道安全描述符";
		return false;
	}
	const HANDLE readyEvent = CreateEventW(&eventSecurity, FALSE, FALSE,
		overlaycontrol::kControlReadyEventName);
	FreeSecurityDescriptor(&eventSecurity);
	if (!readyEvent) {
		UnmapViewOfFile(header);
		CloseHandle(mapping);
		error = "无法创建控制通道事件：" + std::to_string(GetLastError());
		return false;
	}

	ZeroMemory(header, sizeof(*header));
	header->protocolVersion = overlaycontrol::kControlProtocolVersion;
	header->headerSize = sizeof(ControlSharedHeader);
	header->slotCount = overlaycontrol::kControlSlotCount;
	header->slotSize = overlaycontrol::kControlSlotSize;
	header->slotDataOffset = sizeof(ControlSharedHeader);
	header->hubHeartbeatTick = GetTickCount64();

	mapping_ = mapping;
	header_ = header;
	readyEvent_ = readyEvent;
	running_ = true;
	return true;
}

void OverlayControlChannel::Stop() noexcept {
	if (!running_)
		return;
	running_ = false;
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
}

void OverlayControlChannel::KeepAlive() noexcept {
	if (header_)
		header_->hubHeartbeatTick = GetTickCount64();
}

bool OverlayControlChannel::HeartbeatFresh(UINT64 tick) noexcept {
	const UINT64 now = GetTickCount64();
	return tick != 0 && now >= tick &&
		now - tick < overlaycontrol::kControlHeartbeatTimeoutMs;
}

bool OverlayControlChannel::IsDllConnected() const noexcept {
	const ControlSharedHeader* header = header_;
	if (!header)
		return false;
	return HeartbeatFresh(header->dllHeartbeatTick);
}

int OverlayControlChannel::QueryOverlayVisible() const noexcept {
	const ControlSharedHeader* header = header_;
	if (!header || !IsDllConnected())
		return -1;
	const UINT32 state = header->dllOverlayVisible;
	return state == 1 ? 1 : (state == 0 ? 0 : -1);
}

bool OverlayControlChannel::SendSetOverlayVisible(bool visible) noexcept {
	return SendCommand(overlaycontrol::kControlCommandSetOverlayVisible,
		visible ? 1u : 0u);
}

bool OverlayControlChannel::SendScrollAnswer(int direction) noexcept {
	if (direction == 0)
		return true;
	return SendCommand(overlaycontrol::kControlCommandScrollAnswer,
		direction > 0 ? overlaycontrol::kControlScrollDown
			: overlaycontrol::kControlScrollUp);
}

bool OverlayControlChannel::SendCommand(UINT32 type, UINT32 value) noexcept {
	ControlSharedHeader* header = header_;
	if (!running_ || !header)
		return false;
	header->hubHeartbeatTick = GetTickCount64();
	if (!IsDllConnected())
		return false;

	const UINT64 write = header->writeIndex;
	const UINT64 read = header->readIndex;
	if (write - read >= overlaycontrol::kControlSlotCount) {
		// The DLL drains on every wake; a full ring means it is stalled, so
		// drop instead of blocking the UI thread.
		InterlockedIncrement64(
			reinterpret_cast<volatile LONG64*>(&header->droppedByWriter));
		return false;
	}

	UINT8* slot = reinterpret_cast<UINT8*>(header) + sizeof(ControlSharedHeader) +
		(write & (overlaycontrol::kControlSlotCount - 1)) *
			overlaycontrol::kControlSlotSize;
	auto* message = reinterpret_cast<overlaycontrol::ControlCommandMessage*>(slot);
	message->type = type;
	message->value = value;
	message->sequence = ++sequence_;
	message->reserved = 0;
	MemoryBarrier();
	InterlockedIncrement64(
		reinterpret_cast<volatile LONG64*>(&header->writeIndex));
	if (readyEvent_)
		SetEvent(readyEvent_);
	return true;
}

} // namespace coolhelper
