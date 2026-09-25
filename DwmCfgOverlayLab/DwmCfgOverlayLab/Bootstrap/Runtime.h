#pragma once

#include <Windows.h>

#include "../Hooks/HookManager.h"
#include "../IPC/AnswerIpc.h"
#include "../IPC/ControlIpc.h"
#include "../Platform/SystemProbe.h"
#include "../Render/FrameRouter.h"
#include "../Render/OverlayRenderer.h"
#include "InstanceCoordinator.h"

namespace dwm_overlay {

class Runtime final {
public:
	Runtime() noexcept;
	~Runtime() = default;

	bool Initialize() noexcept;
	bool RefreshVmwareProfile() noexcept;
	bool Shutdown() noexcept;
	bool IsInitialized() const noexcept;
	void SetModule(HMODULE module) noexcept { module_ = module; }

private:
	static void OnHook(const HookInvocation& invocation) noexcept;
	static bool CoordinatedShutdownEntry(void* context) noexcept;
	static void OnControlCommand(
		void* context, UINT32 type, UINT32 value) noexcept;
	static bool QueryOverlayVisible(void* context) noexcept;
	void PublishStatus() noexcept;

	SRWLOCK lifecycleLock_ = SRWLOCK_INIT;
	// 0=stopped, 1=starting, 2=owner active, 3=stopping, 4=control client.
	volatile LONG state_ = 0;
	HMODULE module_ = nullptr;
	InstanceCoordinator coordinator_;
	SystemFingerprint fingerprint_ = {};
	HookManager hooks_;
	OverlayRenderer renderer_;
	AnswerIpcService answerIpc_;
	ControlIpcService controlIpc_;
	FrameRouter frameRouter_;
};

Runtime& GetRuntime() noexcept;
bool IsOverlayEnabled() noexcept;

} // namespace dwm_overlay
