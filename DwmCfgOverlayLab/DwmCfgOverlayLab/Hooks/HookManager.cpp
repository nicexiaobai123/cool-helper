#include "HookManager.h"
#include "../Common/Log.h"
#include "../Scanner/PatternScanner.h"

#include <cstring>

namespace dwm_overlay {

static HookManager* volatile g_activeHookManager = nullptr;

HookManager::HookManager() noexcept = default;

bool ValidateLocalCallTarget(
	HMODULE module, UINT64 target, const BytePattern& pattern) noexcept {
	if (!pattern.bytes || !pattern.mask)
		return false;
	const SIZE_T length = std::strlen(pattern.mask);
	ModuleCodeView code = {};
	if (!length || length > 64 || !TryGetModuleCodeView(module, code))
		return false;
	const UINT64 start = reinterpret_cast<UINT64>(code.codeBase);
	if (target < start || target - start > code.codeSize ||
		length > code.codeSize - static_cast<SIZE_T>(target - start) ||
		!IsExecutableAddress(reinterpret_cast<void*>(target)) ||
		!IsReadableRange(reinterpret_cast<void*>(target), length))
		return false;
	SIZE_T fixedBytes = 0;
	for (SIZE_T index = 0; index < length; ++index) {
		if (pattern.mask[index] == 'x') {
			++fixedBytes;
			if (reinterpret_cast<const BYTE*>(target)[index] !=
				static_cast<BYTE>(pattern.bytes[index]))
				return false;
		}
		else if (pattern.mask[index] != '?')
			return false;
	}
	return fixedBytes >= 5;
}

static bool IsOwnedExecutableAddress(
	HMODULE module,
	const void* address) noexcept {
	if (!IsExecutableAddress(address))
		return false;
	MEMORY_BASIC_INFORMATION information = {};
	// Runtime dispatch thunks may occupy a loader-added executable page just
	// beyond the PE's SizeOfImage. VirtualQuery identifies the actual mapping
	// owner; a file-header bounds check incorrectly rejects those thunks.
	return module && VirtualQuery(address, &information, sizeof(information)) &&
		information.AllocationBase == module;
}

static bool IsOwnedExecutableAddress(
	const wchar_t* moduleName,
	const void* address) noexcept {
	return IsOwnedExecutableAddress(GetModuleHandleW(moduleName), address);
}

static bool ValidateFothkCallTarget(
	HMODULE module,
	UINT64 target) noexcept {
	ModuleSectionView fothk = {};
	if (!TryGetModuleSection(module, "fothk", fothk))
		return false;
	const UINT64 sectionStart = reinterpret_cast<UINT64>(fothk.base);
	if (target < sectionStart || target >= sectionStart + fothk.size ||
		!IsReadableRange(reinterpret_cast<const void*>(target), 5) ||
		*reinterpret_cast<const BYTE*>(target) != 0xE9)
		return false;

	const INT32 thunkDisplacement =
		*reinterpret_cast<const INT32*>(target + 1);
	const UINT64 dispatcherThunk = static_cast<UINT64>(
		static_cast<INT64>(target + 5) + thunkDisplacement);
	// The loader can retarget fothk's E9 straight to an ntdll dispatcher.
	// The file image instead points to a module-local FF 25 dispatch thunk.
	if (IsOwnedExecutableAddress(L"ntdll.dll",
		reinterpret_cast<const void*>(dispatcherThunk)))
		return true;
	const void* thunkAddress = reinterpret_cast<const void*>(dispatcherThunk);
	if (IsOwnedExecutableAddress(module, thunkAddress) &&
		IsReadableRange(thunkAddress, 3)) {
		const BYTE* bytes = static_cast<const BYTE*>(thunkAddress);
		// The loader can also append a module-owned no-op dispatch stub.
		if ((bytes[0] == 0xFF && bytes[1] == 0xE0) ||
			(bytes[0] == 0x48 && bytes[1] == 0xFF && bytes[2] == 0xE0))
			return true;
	}
	ModuleCodeView moduleView = {};
	if (!TryGetModuleCodeView(module, moduleView))
		return false;
	const UINT64 moduleStart = reinterpret_cast<UINT64>(moduleView.imageBase);
	const UINT64 moduleEnd = moduleStart + moduleView.imageSize;
	if (dispatcherThunk < moduleStart || dispatcherThunk > moduleEnd - 6)
		return false;
	if (!IsReadableRange(reinterpret_cast<const void*>(dispatcherThunk), 6) ||
		*reinterpret_cast<const WORD*>(dispatcherThunk) != 0x25FF)
		return false;

	const INT32 cellDisplacement =
		*reinterpret_cast<const INT32*>(dispatcherThunk + 2);
	const UINT64 cellAddress = static_cast<UINT64>(
		static_cast<INT64>(dispatcherThunk + 6) + cellDisplacement);
	if (cellAddress < moduleStart || cellAddress > moduleEnd - sizeof(void*) ||
		!IsReadableRange(reinterpret_cast<const void*>(cellAddress),
		sizeof(void*)))
		return false;
	const void* dispatcher = *reinterpret_cast<void* const*>(cellAddress);
	if (IsOwnedExecutableAddress(L"ntdll.dll", dispatcher))
		return true;

	// Before, or without, loader redirection the GuardCF dispatch cell can
	// still reference the module-local no-op dispatcher (jmp rax).
	const UINT64 dispatcherAddress = reinterpret_cast<UINT64>(dispatcher);
	return dispatcherAddress >= moduleStart &&
		dispatcherAddress <= moduleEnd - sizeof(WORD) &&
		IsExecutableAddress(dispatcher) &&
		IsReadableRange(dispatcher, sizeof(WORD)) &&
		*reinterpret_cast<const WORD*>(dispatcher) == 0xE0FF;
}

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

BYTE* HookManager::AllocateFothkRelayNear(
	UINT64 callSite,
	UINT64 originalCallTarget) noexcept {
	SYSTEM_INFO systemInfo = {};
	GetSystemInfo(&systemInfo);
	const SIZE_T granularity = systemInfo.dwAllocationGranularity;
	const UINT64 returnAddress = callSite + 5;
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

			auto relay = reinterpret_cast<BYTE*>(VirtualAlloc(
				reinterpret_cast<void*>(candidate), granularity,
				MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
			if (!relay)
				continue;
			const INT64 displacement = reinterpret_cast<UINT64>(relay) -
				returnAddress;
			if (displacement < INT32_MIN || displacement > INT32_MAX) {
				VirtualFree(relay, 0, MEM_RELEASE);
				continue;
			}

			// call qword ptr [rip+0Ah] -> bridge pointer at +10h
			// jmp  qword ptr [rip+0Ch] -> original fothk pointer at +18h
			static constexpr BYTE relayTemplate[16] = {
				0xFF, 0x15, 0x0A, 0x00, 0x00, 0x00,
				0xFF, 0x25, 0x0C, 0x00, 0x00, 0x00,
				0xCC, 0xCC, 0xCC, 0xCC
			};
			memcpy(relay, relayTemplate, sizeof(relayTemplate));
			const UINT64 bridge = reinterpret_cast<UINT64>(
				AsmFothkCallSiteBridge);
			memcpy(relay + 0x10, &bridge, sizeof(bridge));
			memcpy(relay + 0x18, &originalCallTarget,
				sizeof(originalCallTarget));

			DWORD oldProtection = 0;
			if (!VirtualProtect(relay, granularity, PAGE_EXECUTE_READ,
				&oldProtection)) {
				VirtualFree(relay, 0, MEM_RELEASE);
				return nullptr;
			}
			FlushInstructionCache(GetCurrentProcess(), relay, 0x20);
			return relay;
		}
	}
	return nullptr;
}

bool HookManager::PatchDisplacementAtomic(
	UINT64 callSite,
	CallSiteEncoding encoding,
	INT32 expectedDisplacement,
	INT32 replacementDisplacement) noexcept {
	SIZE_T displacementOffsetFromCall = 0;
	if (encoding == CallSiteEncoding::GuardDispatchRipIndirect) {
		if (*reinterpret_cast<const WORD*>(callSite) != 0x15FF) {
			DWM_LOG("CFG call-site opcode changed before patching");
			return false;
		}
		displacementOffsetFromCall = 2;
	}
	else {
		if (*reinterpret_cast<const BYTE*>(callSite) != 0xE8) {
			DWM_LOG("fothk call-site opcode changed before patching");
			return false;
		}
		displacementOffsetFromCall = 1;
	}
	if (*reinterpret_cast<const INT32*>(
		callSite + displacementOffsetFromCall) != expectedDisplacement) {
		DWM_LOG("Call-site displacement changed before patching");
		return false;
	}

	const UINT64 displacementAddress = callSite + displacementOffsetFromCall;
	const UINT64 patchBlock = displacementAddress & ~0xFull;
	const SIZE_T displacementOffset = static_cast<SIZE_T>(
		displacementAddress - patchBlock);
	if (displacementOffset + sizeof(INT32) > 16) {
		DWM_LOG("Call displacement crosses an atomic patch block");
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
		DWM_LOG("VirtualProtect on call site failed");
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
		DWM_LOG("Call site changed concurrently; patch skipped");
	return patched != FALSE;
}

bool HookManager::Install(
	const HookSpec& specification,
	UINT64 callSite) noexcept {
	if (!callSite)
		return false;
	if (specification.callSiteEncoding == CallSiteEncoding::RelativeCallToVerifiedLocal &&
		specification.kind != PresentKind::DDisplayMultiplaneOverlay) {
		DWM_LOG_FORMAT("%s: local direct-call contract requires the DDisplay adapter", specification.name);
		return false;
	}
	AcquireSRWLockExclusive(&lock_);
	if (FindById(specification.id)) {
		ReleaseSRWLockExclusive(&lock_);
		return true;
	}
	const bool validOpcode = specification.callSiteEncoding ==
		CallSiteEncoding::GuardDispatchRipIndirect
		? *reinterpret_cast<const WORD*>(callSite) == 0x15FF
		: *reinterpret_cast<const BYTE*>(callSite) == 0xE8;
	if (hookCount_ >= kMaximumHooks || !validOpcode) {
		ReleaseSRWLockExclusive(&lock_);
		DWM_LOG_FORMAT("%s: invalid or unavailable hook slot", specification.name);
		return false;
	}

	const SIZE_T displacementOffset = specification.callSiteEncoding ==
		CallSiteEncoding::GuardDispatchRipIndirect ? 2 : 1;
	const SIZE_T instructionLength = specification.callSiteEncoding ==
		CallSiteEncoding::GuardDispatchRipIndirect ? 6 : 5;
	const INT32 originalDisplacement =
		*reinterpret_cast<const INT32*>(callSite + displacementOffset);

	void** dispatchCell = nullptr;
	BYTE* relay = nullptr;
	UINT64 originalCallTarget = 0;
	INT64 displacement64 = 0;
	UINT64 publishedReturnAddress = callSite + instructionLength;
	if (specification.callSiteEncoding ==
		CallSiteEncoding::GuardDispatchRipIndirect) {
		const UINT64 originalCellAddress = static_cast<UINT64>(
			static_cast<INT64>(callSite + instructionLength) +
			originalDisplacement);
		if (!IsReadableRange(reinterpret_cast<const void*>(originalCellAddress),
			sizeof(void*))) {
			ReleaseSRWLockExclusive(&lock_);
			DWM_LOG_FORMAT("%s: CFG dispatcher cell is not readable",
				specification.name);
			return false;
		}
		const void* originalDispatcher =
			*reinterpret_cast<void* const*>(originalCellAddress);
		if (!IsOwnedExecutableAddress(L"ntdll.dll", originalDispatcher)) {
			ReleaseSRWLockExclusive(&lock_);
			DWM_LOG_FORMAT(
				"%s: CFG dispatcher is not owned by ntdll; duplicate/foreign hook rejected",
				specification.name);
			return false;
		}

		dispatchCell = AllocateDispatchCellNear(callSite);
		if (!dispatchCell) {
			ReleaseSRWLockExclusive(&lock_);
			DWM_LOG_FORMAT("%s: unable to allocate nearby dispatch cell",
				specification.name);
			return false;
		}
		*dispatchCell = reinterpret_cast<void*>(AsmLdrpDispatchUserCallTarget);
		MemoryBarrier();
		displacement64 = reinterpret_cast<UINT64>(dispatchCell) -
			(callSite + instructionLength);
	}
	else {
		originalCallTarget = static_cast<UINT64>(
			static_cast<INT64>(callSite + instructionLength) +
			originalDisplacement);
		const HMODULE module = GetModuleHandleW(specification.moduleName);
		const bool validTarget = specification.callSiteEncoding ==
			CallSiteEncoding::RelativeCallToVerifiedLocal
			? ValidateLocalCallTarget(module, originalCallTarget,
				specification.localTargetPattern)
			: ValidateFothkCallTarget(module, originalCallTarget);
		if (!validTarget) {
			ReleaseSRWLockExclusive(&lock_);
			DWM_LOG_FORMAT(
				"%s: original call target/dispatch chain is invalid",
				specification.name);
			return false;
		}
		relay = AllocateFothkRelayNear(callSite, originalCallTarget);
		if (!relay) {
			ReleaseSRWLockExclusive(&lock_);
			DWM_LOG_FORMAT("%s: unable to allocate nearby fothk relay",
				specification.name);
			return false;
		}
		displacement64 = reinterpret_cast<UINT64>(relay) -
			(callSite + instructionLength);
		// The relay's first absolute call returns to its second instruction.
		publishedReturnAddress = reinterpret_cast<UINT64>(relay) + 6;
	}

	if (displacement64 < INT32_MIN || displacement64 > INT32_MAX) {
		if (relay)
			VirtualFree(relay, 0, MEM_RELEASE);
		ReleaseSRWLockExclusive(&lock_);
		return false;
	}

	auto& instance = hooks_[hookCount_];
	instance.specification = &specification;
	instance.callSite = callSite;
	instance.dispatchCell = dispatchCell;
	instance.relayAllocation = relay;
	instance.originalCallTarget = originalCallTarget;
	instance.originalDisplacement = originalDisplacement;
	instance.patchedDisplacement = static_cast<INT32>(displacement64);
	InterlockedExchange64(&instance.returnAddress,
		static_cast<LONG64>(publishedReturnAddress));

	if (!PatchDisplacementAtomic(callSite, specification.callSiteEncoding,
		originalDisplacement,
		instance.patchedDisplacement)) {
		InterlockedExchange64(&instance.returnAddress, 0);
		if (relay)
			VirtualFree(relay, 0, MEM_RELEASE);
		instance = {};
		ReleaseSRWLockExclusive(&lock_);
		return false;
	}
	instance.installed = true;
	++hookCount_;
	ReleaseSRWLockExclusive(&lock_);
	DWM_LOG_FORMAT("Hook installed: %s at %p mode=render", specification.name,
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

UINT64 HookManager::HitCount(HookSiteId id) const noexcept {
	const auto* hook = FindById(id);
	return hook ? static_cast<UINT64>(InterlockedCompareExchange64(
		const_cast<volatile LONG64*>(&hook->hitCount), 0, 0)) : 0;
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
				const LONG64 hits = InterlockedIncrement64(&instance->hitCount);
				const HookInvocation invocation = {
					instance->specification,
					context.returnAddress,
					reinterpret_cast<void*>(ReadArgument(
						instance->specification->argumentSource, context)),
					&context
				};
				if (hits == 1)
					DWM_LOG_FORMAT("Hook first hit: %s argument=%p",
						instance->specification->name, invocation.argument);
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
			hook.specification->callSiteEncoding,
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
		for (auto& hook : hooks_) {
			if (hook.specification)
				DWM_LOG_FORMAT("Hook statistics: %s hits=%llu",
					hook.specification->name,
					static_cast<unsigned long long>(
						InterlockedCompareExchange64(&hook.hitCount, 0, 0)));
			if (hook.relayAllocation)
				VirtualFree(hook.relayAllocation, 0, MEM_RELEASE);
			hook = {};
		}
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
