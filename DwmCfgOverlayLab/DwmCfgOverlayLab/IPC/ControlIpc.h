#pragma once

#include <Windows.h>
#include "../../../Shared/OverlayControlProtocol.h"

namespace dwm_overlay {

// Shared wire contract; do not duplicate layouts between executables.
namespace overlaycontrol = ::coolhelper_overlay;

// Consumes hub control commands. The callbacks are invoked on the worker
// thread and must only touch state that the compositor reads atomically.
class ControlIpcService final {
public:
	struct Callbacks {
		void (*OnCommand)(void* context, UINT32 type, UINT32 value) noexcept;
		bool (*QueryOverlayVisible)(void* context) noexcept;
		UINT32 (*QueryDisplayMode)(void* context) noexcept;
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
	overlaycontrol::ControlSharedHeader* header_ = nullptr;
	bool attachedLogged_ = false;
	UINT32 appliedDisplayMode_ = UINT32_MAX;
};

} // namespace dwm_overlay
