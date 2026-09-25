#pragma once

#include "HookTypes.h"

namespace dwm_overlay {

class HookManager final {
public:
	HookManager() noexcept;
	~HookManager() = default;

	HookManager(const HookManager&) = delete;
	HookManager& operator=(const HookManager&) = delete;

	void SetDispatchCallback(HookDispatchCallback callback) noexcept;
	bool Install(const HookSpec& specification, UINT64 callSite) noexcept;
	bool IsInstalled(HookSiteId id) const noexcept;
	bool UninstallAll(DWORD callbackDrainTimeoutMs = 2000) noexcept;
	void Dispatch(const HookCpuContext& context) noexcept;
	SIZE_T InstalledCount() const noexcept;

	static void SetActiveManager(HookManager* manager) noexcept;
	static HookManager* GetActiveManager() noexcept;

private:
	static constexpr SIZE_T kMaximumHooks = 32;
	static constexpr SIZE_T kMaximumCellArenas = 8;

	struct HookInstance {
		const HookSpec* specification = nullptr;
		UINT64 callSite = 0;
		volatile LONG64 returnAddress = 0;
		void** dispatchCell = nullptr;
		INT32 originalDisplacement = 0;
		INT32 patchedDisplacement = 0;
		volatile LONG64 hitCount = 0;
		bool installed = false;
	};

	struct DispatchCellArena {
		BYTE* base = nullptr;
		SIZE_T size = 0;
		SIZE_T used = 0;
	};

	void** AllocateDispatchCellNear(UINT64 callSite) noexcept;
	bool PatchDisplacementAtomic(
		UINT64 callSite,
		INT32 expectedDisplacement,
		INT32 replacementDisplacement) noexcept;
	HookInstance* FindByReturnAddress(UINT64 returnAddress) noexcept;
	const HookInstance* FindById(HookSiteId id) const noexcept;
	UINT64 ReadArgument(
		ArgumentSource source,
		const HookCpuContext& context) const noexcept;

	mutable SRWLOCK lock_ = SRWLOCK_INIT;
	HookInstance hooks_[kMaximumHooks] = {};
	DispatchCellArena arenas_[kMaximumCellArenas] = {};
	SIZE_T hookCount_ = 0;
	HookDispatchCallback callback_ = nullptr;
	volatile LONG acceptingCallbacks_ = 1;
	volatile LONG activeCallbacks_ = 0;
};

} // namespace dwm_overlay

extern "C" void AsmLdrpDispatchUserCallTarget();
extern "C" void DispatchHook(dwm_overlay::HookCpuContext* context) noexcept;
