#include "InstanceCoordinator.h"
#include "../Common/Log.h"

#include <cwchar>

namespace dwm_overlay {

bool InstanceCoordinator::Initialize(
	HMODULE module,
	void* ownerContext,
	CoordinatedShutdown shutdown) noexcept {
	if (block_)
		return true;

	wchar_t name[96] = {};
	swprintf_s(name, L"Local\\DwmCfgOverlayLab.Control.%lu",
		GetCurrentProcessId());
	mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
		PAGE_READWRITE, 0, sizeof(SharedControlBlock), name);
	if (!mapping_) {
		DWM_LOG("Unable to create the process instance coordinator");
		return false;
	}
	block_ = static_cast<SharedControlBlock*>(MapViewOfFile(
		mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
		sizeof(SharedControlBlock)));
	if (!block_) {
		CloseHandle(mapping_);
		mapping_ = nullptr;
		DWM_LOG("Unable to map the process instance coordinator");
		return false;
	}

	for (int attempt = 0; attempt < 5000; ++attempt) {
		const LONG state = InterlockedCompareExchange(&block_->state, 1, 0);
		if (state == 0) {
			block_->ownerContext = ownerContext;
			block_->shutdown = shutdown;
			block_->ownerModule = module;
			MemoryBarrier();
			role_ = Role::Owner;
			DWM_LOG("Primary overlay instance registered");
			return true;
		}
		if (state == 2 || state == 3) {
			role_ = Role::ControlClient;
			DWM_LOG("Existing overlay instance detected; control forwarding enabled");
			return true;
		}
		Sleep(1);
	}

	DWM_LOG("Overlay instance registration timed out");
	Close();
	return false;
}

void InstanceCoordinator::PublishOwnerReady() noexcept {
	if (!block_ || role_ != Role::Owner)
		return;
	MemoryBarrier();
	InterlockedCompareExchange(&block_->state, 2, 1);
}

bool InstanceCoordinator::ForwardShutdown() noexcept {
	if (!block_ || role_ != Role::ControlClient)
		return false;
	if (InterlockedCompareExchange(&block_->state, 0, 0) != 2)
		return false;
	MemoryBarrier();
	const auto shutdown = block_->shutdown;
	void* context = block_->ownerContext;
	const HMODULE ownerModule = block_->ownerModule;
	HMODULE resolvedModule = nullptr;
	if (!shutdown || !context || !ownerModule ||
		!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(shutdown), &resolvedModule) ||
		resolvedModule != ownerModule) {
		DWM_LOG("Primary overlay coordinator contains a stale callback");
		return false;
	}
	DWM_LOG("Forwarding shutdown to the primary overlay instance");
	return shutdown(context);
}

void InstanceCoordinator::ReleaseOwner() noexcept {
	if (!block_ || role_ != Role::Owner)
		return;
	InterlockedExchange(&block_->state, 3);
	MemoryBarrier();
	block_->shutdown = nullptr;
	block_->ownerContext = nullptr;
	block_->ownerModule = nullptr;
	MemoryBarrier();
	InterlockedExchange(&block_->state, 0);
}

void InstanceCoordinator::Close() noexcept {
	if (block_) {
		UnmapViewOfFile(block_);
		block_ = nullptr;
	}
	if (mapping_) {
		CloseHandle(mapping_);
		mapping_ = nullptr;
	}
	role_ = Role::Uninitialized;
}

} // namespace dwm_overlay
