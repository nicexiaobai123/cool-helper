#include "ControlIpc.h"

#include "../Common/Log.h"

namespace dwm_overlay {

ControlIpcService::ControlIpcService(const Callbacks& callbacks) noexcept
	: callbacks_(callbacks) {}

void ControlIpcService::Start() noexcept {
	if (workerThread_)
		return;
	if (!stopEvent_) {
		stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!stopEvent_) {
			DWM_LOG("Control IPC stop event could not be created");
			return;
		}
	}
	ResetEvent(stopEvent_);
	const HANDLE thread = CreateThread(nullptr, 0, WorkerProc, this, 0, nullptr);
	if (thread)
		workerThread_ = thread;
	else
		DWM_LOG("Control IPC worker thread could not be created");
}

void ControlIpcService::Stop() noexcept {
	if (stopEvent_)
		SetEvent(stopEvent_);
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

DWORD WINAPI ControlIpcService::WorkerProc(void* context) noexcept {
	static_cast<ControlIpcService*>(context)->WorkerLoop();
	return 0;
}

bool ControlIpcService::HeartbeatFresh(UINT64 tick) noexcept {
	const UINT64 now = GetTickCount64();
	return tick != 0 && now >= tick && now - tick < kControlHeartbeatTimeoutMs;
}

void ControlIpcService::WorkerLoop() noexcept {
	for (;;) {
		if (!stopEvent_)
			break;
		if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0)
			break;

		if (!header_ && !TryAttach()) {
			if (WaitForSingleObject(stopEvent_, 500) == WAIT_OBJECT_0)
				break;
			continue;
		}

		Drain();

		if (HeartbeatFresh(header_->hubHeartbeatTick)) {
			header_->dllHeartbeatTick = GetTickCount64();
			const bool visible = !callbacks_.QueryOverlayVisible ||
				callbacks_.QueryOverlayVisible(callbacks_.context);
			header_->dllOverlayVisible = visible ? 1 : 0;
		}
		else {
			Detach("hub heartbeat lost");
			continue;
		}

		HANDLE waits[2] = { readyEvent_, stopEvent_ };
		const DWORD result = WaitForMultipleObjects(2, waits, FALSE, 500);
		if (result == WAIT_OBJECT_0 + 1)
			break;
	}
}

bool ControlIpcService::TryAttach() noexcept {
	const HANDLE mapping = OpenFileMappingW(
		FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kControlSectionName);
	if (!mapping)
		return false;
	ControlSharedHeader* view = static_cast<ControlSharedHeader*>(MapViewOfFile(
		mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
	if (!view) {
		CloseHandle(mapping);
		return false;
	}
	if (view->protocolVersion != kControlProtocolVersion ||
		view->headerSize != sizeof(ControlSharedHeader) ||
		view->slotCount != kControlSlotCount ||
		view->slotSize != kControlSlotSize) {
		DWM_LOG("Control IPC version mismatch; waiting for a compatible hub");
		UnmapViewOfFile(view);
		CloseHandle(mapping);
		Sleep(2000);
		return false;
	}
	const HANDLE readyEvent = OpenEventW(SYNCHRONIZE, FALSE, kControlReadyEventName);
	if (!readyEvent) {
		UnmapViewOfFile(view);
		CloseHandle(mapping);
		return false;
	}

	mapping_ = mapping;
	header_ = view;
	readyEvent_ = readyEvent;
	header_->readIndex = 0;
	header_->dllOverlayVisible = callbacks_.QueryOverlayVisible &&
		callbacks_.QueryOverlayVisible(callbacks_.context) ? 1 : 0;
	header_->dllHeartbeatTick = GetTickCount64();
	if (!attachedLogged_) {
		DWM_LOG("Control IPC attached to CoolHelperHub");
		attachedLogged_ = true;
	}
	return true;
}

void ControlIpcService::Detach(const char* reason) noexcept {
	if (header_) {
		DWM_LOG_FORMAT("Control IPC detached: %s", reason ? reason : "unknown");
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
}

void ControlIpcService::Drain() noexcept {
	ControlSharedHeader* header = header_;
	if (!header)
		return;
	const UINT64 write = header->writeIndex;
	UINT64 read = header->readIndex;
	if (read >= write)
		return;

	const UINT8* slots = reinterpret_cast<const UINT8*>(header) +
		header->slotDataOffset;
	while (read < write) {
		const UINT8* slot = slots + (read & (kControlSlotCount - 1)) * kControlSlotSize;
		const auto message = reinterpret_cast<const ControlCommandMessage*>(slot);
		if (message->type != 0 && callbacks_.OnCommand) {
			callbacks_.OnCommand(callbacks_.context,
				message->type, message->value);
		}
		MemoryBarrier();
		header->readIndex = ++read;
	}
}

} // namespace dwm_overlay
