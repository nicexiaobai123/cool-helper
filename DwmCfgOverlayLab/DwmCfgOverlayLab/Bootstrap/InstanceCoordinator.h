#pragma once

#include <Windows.h>

namespace dwm_overlay {

using CoordinatedShutdown = bool(*)(void* context) noexcept;

class InstanceCoordinator final {
public:
	enum class Role {
		Uninitialized,
		Owner,
		ControlClient
	};

	bool Initialize(
		HMODULE module,
		void* ownerContext,
		CoordinatedShutdown shutdown) noexcept;
	void PublishOwnerReady() noexcept;
	bool ForwardShutdown() noexcept;
	void ReleaseOwner() noexcept;
	void Close() noexcept;
	Role GetRole() const noexcept { return role_; }

private:
	struct SharedControlBlock {
		volatile LONG state; // 0=empty, 1=registering, 2=active, 3=stopping
		void* ownerContext;
		CoordinatedShutdown shutdown;
		HMODULE ownerModule;
	};

	HANDLE mapping_ = nullptr;
	SharedControlBlock* block_ = nullptr;
	Role role_ = Role::Uninitialized;
};

} // namespace dwm_overlay
