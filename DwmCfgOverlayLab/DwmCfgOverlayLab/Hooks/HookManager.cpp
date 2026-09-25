#include "HookManager.h"
#include "../Common/Log.h"
#include "../Scanner/PatternScanner.h"

#include <cstring>

namespace dwm_overlay {

static HookManager* volatile g_activeHookManager = nullptr;

HookManager::HookManager() noexcept = default;

void HookManager::SetActiveManager(HookManager* manager) noexcept {
	InterlockedExchangePointer(
		reinterpret_cast<void* volatile*>(&g_activeHookManager), manager);
}

HookManager* HookManager::GetActiveManager() noexcept {
	return reinterpret_cast<HookManager*>(InterlockedCompareExchangePointer(
		reinterpret_cast<void* volatile*>(&g_activeHookManager), nullptr, nullptr));
}

void HookManager::SetDispatchCallback(HookDispatchCallback callback) noexcept {
	callback_ = callback;
	MemoryBarrier();
}

void** HookManager::AllocateDispatchCellNear(UINT64 callSite) noexcept {
	SYSTEM_INFO systemInfo = {};
	GetSystemInfo(&systemInfo);
	const SIZE_T granularity = systemInfo.dwAllocationGranularity;
	const UINT64 returnAddress = callSite + 6;

	for (auto& arena : arenas_) {
		if (!arena.base || arena.used + sizeof(void*) > arena.size)
			continue;
		auto cell = reinterpret_cast<void**>(arena.base + arena.used);
		const INT64 displacement = reinterpret_cast<UINT64>(cell) - returnAddress;
		if (displacement < INT32_MIN || displacement > INT32_MAX)
			continue;
		arena.used += sizeof(void*);
		return cell;
	}

	DispatchCellArena* freeArena = nullptr;
	for (auto& arena : arenas_) {
		if (!arena.base) {
			freeArena = &arena;
			break;
		}
	}
	if (!freeArena)
		return nullptr;

	const UINT64 center = callSite & ~(static_cast<UINT64>(granularity) - 1);
	const UINT64 maximumDistance = 0x7FFF0000ull;
	for (UINT64 distance = granularity;
		distance <= maximumDistance;
		distance += granularity) {
		const UINT64 candidates[] = {
			center >= distance ? center - distance : 0,
			center + distance
		};
		for (const UINT64 candidate : candidates) {
			if (!candidate ||
				candidate < reinterpret_cast<UINT64>(
					systemInfo.lpMinimumApplicationAddress) ||
				candidate > reinterpret_cast<UINT64>(
					systemInfo.lpMaximumApplicationAddress))
				continue;

			auto memory = reinterpret_cast<BYTE*>(VirtualAlloc(
				reinterpret_cast<void*>(candidate), granularity,
				MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
			if (!memory)
				continue;
			const INT64 displacement = reinterpret_cast<UINT64>(memory) -
				returnAddress;
			if (displacement < INT32_MIN || displacement > INT32_MAX) {
				VirtualFree(memory, 0, MEM_RELEASE);
				continue;
			}

			freeArena->base = memory;
			freeArena->size = granularity;
			freeArena->used = sizeof(void*);
			return reinterpret_cast<void**>(memory);
		}
	}
	return nullptr;
}

bool HookManager::PatchDisplacementAtomic(
	UINT64 callSite,
	INT32 expectedDisplacement,
	INT32 replacementDisplacement) noexcept {
	if (*reinterpret_cast<const WORD*>(callSite) != 0x15FF) {
		DWM_LOG("CFG call-site opcode changed before patching");
		return false;
	}
	if (*reinterpret_cast<const INT32*>(callSite + 2) != expectedDisplacement) {
		DWM_LOG("CFG call-site displacement changed before patching");
		return false;
	}

	const UINT64 displacementAddress = callSite + 2;
	const UINT64 patchBlock = displacementAddress & ~0xFull;
	const SIZE_T displacementOffset = static_cast<SIZE_T>(
		displacementAddress - patchBlock);
	if (displacementOffset + sizeof(INT32) > 16) {
		DWM_LOG("CFG call displacement crosses an atomic patch block");
		return false;
	}

	alignas(16) LONG64 expected[2] = {};
	alignas(16) LONG64 replacement[2] = {};
	memcpy(expected, reinterpret_cast<const void*>(patchBlock), sizeof(expected));
	memcpy(replacement, expected, sizeof(replacement));
	memcpy(reinterpret_cast<BYTE*>(replacement) + displacementOffset,
		&replacementDisplacement, sizeof(replacementDisplacement));

	DWORD oldProtection = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(patchBlock), sizeof(replacement),
		PAGE_EXECUTE_READWRITE, &oldProtection)) {
		DWM_LOG("VirtualProtect on CFG call site failed");
		return false;
	}

	MemoryBarrier();
	const BOOLEAN patched = InterlockedCompareExchange128(
		reinterpret_cast<volatile LONG64*>(patchBlock),
		replacement[1], replacement[0], expected);
	if (patched) {
		FlushInstructionCache(GetCurrentProcess(),
			reinterpret_cast<void*>(patchBlock), sizeof(replacement));
	}
	DWORD ignored = 0;
	VirtualProtect(reinterpret_cast<void*>(patchBlock), sizeof(replacement),
		oldProtection, &ignored);
	if (!patched)
		DWM_LOG("CFG call site changed concurrently; patch skipped");
	return patched != FALSE;
}

bool HookManager::Install(
	const HookSpec& specification,
	UINT64 callSite) noexcept {
	if (!callSite)
		return false;
	AcquireSRWLockExclusive(&lock_);
	if (FindById(specification.id)) {
		ReleaseSRWLockExclusive(&lock_);
		return true;
	}
	if (hookCount_ >= kMaximumHooks ||
		*reinterpret_cast<const WORD*>(callSite) != 0x15FF) {
		ReleaseSRWLockExclusive(&lock_);
		DWM_LOG_FORMAT("%s: invalid or unavailable hook slot", specification.name);
		return false;
	}

	const INT32 originalDisplacement =
		*reinterpret_cast<const INT32*>(callSite + 2);
	const UINT64 originalCellAddress = callSite + 6 + originalDisplacement;
	if (!IsReadableRange(reinterpret_cast<const void*>(originalCellAddress),
		sizeof(void*))) {
		ReleaseSRWLockExclusive(&lock_);
		DWM_LOG_FORMAT("%s: CFG dispatcher cell is not readable",
			specification.name);
		return false;
	}
	const void* originalDispatcher =
		*reinterpret_cast<void* const*>(originalCellAddress);
	if (!IsExecutableAddress(originalDispatcher)) {
		ReleaseSRWLockExclusive(&lock_);
		DWM_LOG_FORMAT("%s: CFG dispatcher target is not executable",
			specification.name);
		return false;
	}
	ModuleCodeView ntdllView = {};
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	const UINT64 dispatcherAddress =
		reinterpret_cast<UINT64>(originalDispatcher);
	if (!TryGetModuleCodeView(ntdll, ntdllView) ||
		dispatcherAddress < reinterpret_cast<UINT64>(ntdllView.imageBase) ||
		dispatcherAddress >= reinterpret_cast<UINT64>(ntdllView.imageBase) +
			ntdllView.imageSize) {
		ReleaseSRWLockExclusive(&lock_);
		DWM_LOG_FORMAT(
			"%s: CFG dispatcher is not owned by ntdll; duplicate/foreign hook rejected",
			specification.name);
		return false;
	}

	void** dispatchCell = AllocateDispatchCellNear(callSite);
	if (!dispatchCell) {
		ReleaseSRWLockExclusive(&lock_);
		DWM_LOG_FORMAT("%s: unable to allocate nearby dispatch cell",
			specification.name);
		return false;
	}
	*dispatchCell = reinterpret_cast<void*>(AsmLdrpDispatchUserCallTarget);
	MemoryBarrier();

	const INT64 displacement64 = reinterpret_cast<UINT64>(dispatchCell) -
		(callSite + 6);
	if (displacement64 < INT32_MIN || displacement64 > INT32_MAX) {
		ReleaseSRWLockExclusive(&lock_);
		return false;
	}

	auto& instance = hooks_[hookCount_];
	instance.specification = &specification;
	instance.callSite = callSite;
	instance.dispatchCell = dispatchCell;
	instance.originalDisplacement = originalDisplacement;
	instance.patchedDisplacement = static_cast<INT32>(displacement64);
	InterlockedExchange64(&instance.returnAddress,
		static_cast<LONG64>(callSite + 6));

	if (!PatchDisplacementAtomic(callSite, originalDisplacement,
		instance.patchedDisplacement)) {
		InterlockedExchange64(&instance.returnAddress, 0);
		instance = {};
		ReleaseSRWLockExclusive(&lock_);
		return false;
	}
	instance.installed = true;
	++hookCount_;
	ReleaseSRWLockExclusive(&lock_);
	DWM_LOG_FORMAT("Hook installed: %s at %p", specification.name,
		reinterpret_cast<void*>(callSite));
	return true;
}

HookManager::HookInstance* HookManager::FindByReturnAddress(
	UINT64 returnAddress) noexcept {
	for (auto& hook : hooks_) {
		const UINT64 published = static_cast<UINT64>(
			InterlockedCompareExchange64(&hook.returnAddress, 0, 0));
		if (published == returnAddress)
			return &hook;
	}
	return nullptr;
}

const HookManager::HookInstance* HookManager::FindById(
	HookSiteId id) const noexcept {
	for (const auto& hook : hooks_) {
		if (hook.installed && hook.specification &&
			hook.specification->id == id)
			return &hook;
	}
	return nullptr;
}

bool HookManager::IsInstalled(HookSiteId id) const noexcept {
	return FindById(id) != nullptr;
}

SIZE_T HookManager::InstalledCount() const noexcept {
	return hookCount_;
}

UINT64 HookManager::ReadArgument(
	ArgumentSource source,
	const HookCpuContext& context) const noexcept {
	switch (source) {
	case ArgumentSource::Rcx: return context.rcx;
	case ArgumentSource::Rdx: return context.rdx;
	case ArgumentSource::R8: return context.r8;
	case ArgumentSource::R9: return context.r9;
	default: return 0;
	}
}

void HookManager::Dispatch(const HookCpuContext& context) noexcept {
	InterlockedIncrement(&activeCallbacks_);
	__try {
		if (InterlockedCompareExchange(&acceptingCallbacks_, 0, 0) != 0) {
			auto instance = FindByReturnAddress(context.returnAddress);
			if (instance && instance->specification) {
				InterlockedIncrement64(&instance->hitCount);
				const HookInvocation invocation = {
					instance->specification,
					context.returnAddress,
					reinterpret_cast<void*>(ReadArgument(
						instance->specification->argumentSource, context))
				};
				const auto callback = callback_;
				if (callback)
					callback(invocation);
			}
		}
	}
	__finally {
		InterlockedDecrement(&activeCallbacks_);
	}
}

bool HookManager::UninstallAll(DWORD callbackDrainTimeoutMs) noexcept {
	InterlockedExchange(&acceptingCallbacks_, 0);
	AcquireSRWLockExclusive(&lock_);
	bool restoredAll = true;
	for (SIZE_T index = hookCount_; index > 0; --index) {
		auto& hook = hooks_[index - 1];
		if (!hook.installed)
			continue;
		if (!PatchDisplacementAtomic(hook.callSite,
			hook.patchedDisplacement, hook.originalDisplacement)) {
			restoredAll = false;
			continue;
		}
		hook.installed = false;
		InterlockedExchange64(&hook.returnAddress, 0);
	}
	ReleaseSRWLockExclusive(&lock_);

	const ULONGLONG deadline = GetTickCount64() + callbackDrainTimeoutMs;
	while (InterlockedCompareExchange(&activeCallbacks_, 0, 0) != 0 &&
		GetTickCount64() < deadline)
		Sleep(1);
	if (InterlockedCompareExchange(&activeCallbacks_, 0, 0) != 0)
		restoredAll = false;

	if (restoredAll) {
		for (auto& arena : arenas_) {
			if (arena.base)
				VirtualFree(arena.base, 0, MEM_RELEASE);
			arena = {};
		}
		hookCount_ = 0;
	}
	return restoredAll;
}

} // namespace dwm_overlay

static void DispatchHookProtected(dwm_overlay::HookCpuContext* context) noexcept {
	if (!context)
		return;
	auto manager = dwm_overlay::HookManager::GetActiveManager();
	if (manager)
		manager->Dispatch(*context);
}

extern "C" void DispatchHook(dwm_overlay::HookCpuContext* context) noexcept {
	__try {
		DispatchHookProtected(context);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		DWM_LOG_ONCE("Overlay callback raised an exception; original Present continued");
	}
}
