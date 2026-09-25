#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dwm_overlay {

struct BytePattern {
	const char* bytes = nullptr;
	const char* mask = nullptr;
};

struct ModuleCodeView {
	const BYTE* imageBase = nullptr;
	SIZE_T imageSize = 0;
	const BYTE* codeBase = nullptr;
	SIZE_T codeSize = 0;
};

bool TryGetModuleCodeView(HMODULE module, ModuleCodeView& view) noexcept;
std::vector<UINT64> FindPatternMatches(
	UINT64 address,
	SIZE_T size,
	const BytePattern& pattern,
	SIZE_T maximumMatches = 0);
UINT64 FindUniquePatternInCode(
	HMODULE module,
	const BytePattern& pattern,
	const char* diagnosticName) noexcept;
std::vector<UINT64> FindPatternMatchesInRange(
	UINT64 address,
	SIZE_T size,
	const BytePattern& pattern,
	SIZE_T maximumMatches = 0);
bool IsExecutableAddress(const void* address) noexcept;
bool IsReadableRange(const void* address, SIZE_T size) noexcept;

} // namespace dwm_overlay
