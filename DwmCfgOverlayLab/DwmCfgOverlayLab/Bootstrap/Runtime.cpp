#include "Runtime.h"
#include "../Common/Log.h"
#include "../Profiles/DwmHookProfiles.h"

#include <cstdio>

namespace dwm_overlay {

Runtime::Runtime() noexcept
	: controlIpc_(ControlIpcService::Callbacks{
		  &Runtime::OnControlCommand, &Runtime::QueryOverlayVisible, this }),
	frameRouter_(renderer_) {}

void Runtime::OnControlCommand(
	void* context, UINT32 type, UINT32 value) noexcept {
	auto* runtime = static_cast<Runtime*>(context);
	if (!runtime || !runtime->IsInitialized())
		return;
	if (type == kControlCommandSetOverlayVisible)
		runtime->renderer_.SetOverlayVisible(value != 0);
	else if (type == kControlCommandScrollAnswer &&
		(value == kControlScrollUp || value == kControlScrollDown))
		runtime->renderer_.RequestAnswerScroll(
			value == kControlScrollDown ? 1 : -1);
}

bool Runtime::QueryOverlayVisible(void* context) noexcept {
	auto* runtime = static_cast<Runtime*>(context);
	return runtime && runtime->renderer_.IsOverlayVisible();
}

Runtime& GetRuntime() noexcept {
	// Explicit Shutdown owns teardown and avoids CRT destructor work while the
	// Windows loader lock is held.
	static Runtime* runtime = new Runtime();
	return *runtime;
}

bool IsOverlayEnabled() noexcept {
	static LONG state = -1;
	if (state == -1) {
		char value[8] = {};
		const DWORD length = GetEnvironmentVariableA(
			"DWM_CFG_OVERLAY_ENABLE", value, sizeof(value));
		const bool disabled = length > 0 && value[0] == '0';
		InterlockedExchange(&state, disabled ? 0 : 1);
	}
	return InterlockedCompareExchange(&state, 0, 0) == 1;
}

bool Runtime::IsInitialized() const noexcept {
	return InterlockedCompareExchange(
		const_cast<volatile LONG*>(&state_), 0, 0) == 2;
}

void Runtime::OnHook(const HookInvocation& invocation) noexcept {
	auto& runtime = GetRuntime();
	if (runtime.IsInitialized())
		runtime.frameRouter_.Route(invocation);
}

bool Runtime::CoordinatedShutdownEntry(void* context) noexcept {
	if (!context)
		return false;
	return static_cast<Runtime*>(context)->Shutdown();
}

void Runtime::PublishStatus() noexcept {
	UiSnapshot snapshot = {};
	snapshot.revision = 1;
	_snprintf_s(snapshot.status, _countof(snapshot.status), _TRUNCATE,
		"Windows %lu | dwmcore %u.%u.%u.%u | hooks %zu",
		fingerprint_.osBuild,
		fingerprint_.dwmcoreVersion.major,
		fingerprint_.dwmcoreVersion.minor,
		fingerprint_.dwmcoreVersion.build,
		fingerprint_.dwmcoreVersion.revision,
		hooks_.InstalledCount());
	renderer_.StateStore().Publish(snapshot);
}

bool Runtime::Initialize() noexcept {
	AcquireSRWLockExclusive(&lifecycleLock_);
	if (InterlockedCompareExchange(&state_, 1, 0) != 0) {
		const bool initialized = IsInitialized();
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return initialized;
	}
	DWM_LOG("Runtime initialization begin");
	if (!IsOverlayEnabled()) {
		DWM_LOG("Overlay disabled by DWM_CFG_OVERLAY_ENABLE=0");
		InterlockedExchange(&state_, 0);
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return false;
	}
	if (!coordinator_.Initialize(
		module_, this, CoordinatedShutdownEntry)) {
		InterlockedExchange(&state_, 0);
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return false;
	}
	if (coordinator_.GetRole() == InstanceCoordinator::Role::ControlClient) {
		InterlockedExchange(&state_, 4);
		DWM_LOG("Control-only DLL instance ready; hooks were not installed");
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return false;
	}

	renderer_.Initialize();
	renderer_.SetAnswerProvider(&answerIpc_);
	hooks_.SetDispatchCallback(OnHook);
	HookManager::SetActiveManager(&hooks_);
	fingerprint_ = ProbeSystem();
	const auto result = InstallCompatibleHookProfiles(fingerprint_, hooks_);
	if (!result.installedHooks) {
		DWM_LOG("No verified CFG call-site hook could be installed");
		HookManager::SetActiveManager(nullptr);
		renderer_.Shutdown();
		coordinator_.ReleaseOwner();
		coordinator_.Close();
		InterlockedExchange(&state_, 0);
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return false;
	}

	PublishStatus();
	InterlockedExchange(&state_, 2);
	answerIpc_.Start();
	controlIpc_.Start();
	coordinator_.PublishOwnerReady();
	DWM_LOG_FORMAT("Runtime ready with %zu hook(s)", hooks_.InstalledCount());
	ReleaseSRWLockExclusive(&lifecycleLock_);
	return true;
}

bool Runtime::RefreshVmwareProfile() noexcept {
	if (!IsVmwareDwm())
		return false;
	AcquireSRWLockExclusive(&lifecycleLock_);
	if (!IsInitialized() || fingerprint_.vmwareD3D) {
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return false;
	}
	fingerprint_ = ProbeSystem();
	const SIZE_T before = hooks_.InstalledCount();
	InstallCompatibleHookProfiles(fingerprint_, hooks_);
	PublishStatus();
	const bool installed = hooks_.InstalledCount() > before;
	ReleaseSRWLockExclusive(&lifecycleLock_);
	return installed;
}

bool Runtime::Shutdown() noexcept {
	AcquireSRWLockExclusive(&lifecycleLock_);
	if (InterlockedCompareExchange(&state_, 0, 0) == 4) {
		InterlockedExchange(&state_, 3);
		const bool forwarded = coordinator_.ForwardShutdown();
		coordinator_.Close();
		InterlockedExchange(&state_, 0);
		DWM_LOG(forwarded
			? "Primary overlay shutdown completed through control instance"
			: "Primary overlay shutdown forwarding failed");
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return forwarded;
	}
	if (InterlockedCompareExchange(&state_, 3, 2) != 2) {
		const bool stopped = InterlockedCompareExchange(&state_, 0, 0) == 0;
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return stopped;
	}
	DWM_LOG("Runtime shutdown begin");
	controlIpc_.Stop();
	answerIpc_.Stop();
	const bool hooksRemoved = hooks_.UninstallAll();
	if (!hooksRemoved) {
		DWM_LOG("One or more hooks could not be safely removed; DLL must remain loaded");
		ReleaseSRWLockExclusive(&lifecycleLock_);
		return false;
	}
	HookManager::SetActiveManager(nullptr);
	renderer_.Shutdown();
	coordinator_.ReleaseOwner();
	coordinator_.Close();
	InterlockedExchange(&state_, 0);
	DWM_LOG("Runtime shutdown complete");
	ReleaseSRWLockExclusive(&lifecycleLock_);
	return true;
}

} // namespace dwm_overlay
