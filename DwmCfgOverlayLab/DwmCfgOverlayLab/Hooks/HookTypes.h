#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

#include "../Scanner/PatternScanner.h"

namespace dwm_overlay {

enum class HookSiteId : UINT32 {
	Win10DevicePresent,
	Win10Present1,
	Win10PresentMpo,
	Win10LegacyD2DPresent,
	Win10LegacyD2DPresentMpo,
	Win10VmwarePresentInternal,
	ExperimentalD2DPresent
};

enum class ArgumentSource : UINT8 {
	Rcx,
	Rdx,
	R8,
	R9
};

enum class PresentKind : UINT8 {
	DxgiPresent,
	DxgiPresent1,
	MultiplaneOverlay,
	VmwarePresentInternal
};

enum class EnvironmentRequirement : UINT8 {
	Any,
	VmwareD3D
};

struct PatternVariant {
	BytePattern functionPattern;
	SIZE_T callSearchOffset;
	SIZE_T callSearchLength;
	SIZE_T callOrdinal;
	bool requireUniqueCall;
	const char* variantName;
};

struct HookSpec {
	HookSiteId id;
	const char* name;
	const wchar_t* moduleName;
	DWORD minimumBuild;
	DWORD maximumBuild;
	WORD minimumModuleBuild;
	WORD maximumModuleBuild;
	EnvironmentRequirement environment;
	ArgumentSource argumentSource;
	PresentKind kind;
	const PatternVariant* variants;
	SIZE_T variantCount;
};

// This structure mirrors the register save area emitted by HookBridge.asm.
// Keeping the assembly generic means that adding RCX/RDX/R8/R9 based paths
// only requires a new HookSpec, not another assembly branch.
struct HookCpuContext {
	UINT64 rax;
	UINT64 rcx;
	UINT64 rdx;
	UINT64 rbx;
	UINT64 rbp;
	UINT64 rsi;
	UINT64 rdi;
	UINT64 r8;
	UINT64 r9;
	UINT64 r10;
	UINT64 r11;
	UINT64 r12;
	UINT64 r13;
	UINT64 r14;
	UINT64 r15;
	BYTE registerPadding[8];
	BYTE xmm[6][16];
	BYTE contextPadding[0x20];
	UINT64 returnAddress;
};

static_assert(offsetof(HookCpuContext, rcx) == 0x08, "RCX offset mismatch");
static_assert(offsetof(HookCpuContext, rdx) == 0x10, "RDX offset mismatch");
static_assert(offsetof(HookCpuContext, r8) == 0x38, "R8 offset mismatch");
static_assert(offsetof(HookCpuContext, r9) == 0x40, "R9 offset mismatch");
static_assert(offsetof(HookCpuContext, xmm) == 0x80, "XMM offset mismatch");
static_assert(offsetof(HookCpuContext, returnAddress) == 0x100,
	"Return-address offset mismatch");

struct HookInvocation {
	const HookSpec* specification;
	UINT64 returnAddress;
	void* argument;
};

using HookDispatchCallback = void(*)(const HookInvocation& invocation) noexcept;

} // namespace dwm_overlay
