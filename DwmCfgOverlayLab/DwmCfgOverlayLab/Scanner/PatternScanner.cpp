#include "PatternScanner.h"
#include "../Common/Log.h"

#include <cstring>

namespace dwm_overlay {

static bool PatternMatches(
	const BYTE* data,
	const BytePattern& pattern,
	SIZE_T length) noexcept {
	for (SIZE_T i = 0; i < length; ++i) {
		if (pattern.mask[i] != '?' &&
			data[i] != static_cast<BYTE>(pattern.bytes[i]))
			return false;
	}
	return true;
}

bool TryGetModuleCodeView(HMODULE module, ModuleCodeView& view) noexcept {
	view = {};
	if (!module)
		return false;

	const auto base = reinterpret_cast<const BYTE*>(module);
	const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
		return false;
	const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
		base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		return false;

	view.imageBase = base;
	view.imageSize = nt->OptionalHeader.SizeOfImage;
	const auto sections = IMAGE_FIRST_SECTION(nt);
	for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
		char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
		memcpy(name, sections[index].Name, IMAGE_SIZEOF_SHORT_NAME);
		if (strcmp(name, ".text") != 0)
			continue;

		const SIZE_T virtualAddress = sections[index].VirtualAddress;
		const SIZE_T virtualSize = sections[index].Misc.VirtualSize;
		if (!virtualSize || virtualAddress >= view.imageSize ||
			virtualSize > view.imageSize - virtualAddress)
			return false;
		view.codeBase = base + virtualAddress;
		view.codeSize = virtualSize;
		return true;
	}
	return false;
}

std::vector<UINT64> FindPatternMatches(
	UINT64 address,
	SIZE_T size,
	const BytePattern& pattern,
	SIZE_T maximumMatches) {
	std::vector<UINT64> matches;
	if (!address || !pattern.bytes || !pattern.mask)
		return matches;
	const SIZE_T patternLength = strlen(pattern.mask);
	if (!patternLength || patternLength > size)
		return matches;

	const auto data = reinterpret_cast<const BYTE*>(address);
	const SIZE_T finalOffset = size - patternLength;
	for (SIZE_T offset = 0; offset <= finalOffset; ++offset) {
		if (!PatternMatches(data + offset, pattern, patternLength))
			continue;
		matches.push_back(address + offset);
		if (maximumMatches && matches.size() >= maximumMatches)
			break;
	}
	return matches;
}

std::vector<UINT64> FindPatternMatchesInRange(
	UINT64 address,
	SIZE_T size,
	const BytePattern& pattern,
	SIZE_T maximumMatches) {
	return FindPatternMatches(address, size, pattern, maximumMatches);
}

UINT64 FindUniquePatternInCode(
	HMODULE module,
	const BytePattern& pattern,
	const char* diagnosticName) noexcept {
	ModuleCodeView view = {};
	if (!TryGetModuleCodeView(module, view)) {
		DWM_LOG_FORMAT("%s: invalid module image", diagnosticName);
		return 0;
	}

	try {
		const auto matches = FindPatternMatches(
			reinterpret_cast<UINT64>(view.codeBase), view.codeSize,
			pattern, 2);
		if (matches.empty()) {
			DWM_LOG_FORMAT("%s pattern not found", diagnosticName);
			return 0;
		}
		if (matches.size() != 1) {
			DWM_LOG_FORMAT("%s pattern is ambiguous", diagnosticName);
			return 0;
		}
		return matches.front();
	}
	catch (...) {
		DWM_LOG_FORMAT("%s pattern scan failed", diagnosticName);
		return 0;
	}
}

bool IsExecutableAddress(const void* address) noexcept {
	MEMORY_BASIC_INFORMATION information = {};
	if (!address || !VirtualQuery(address, &information, sizeof(information)) ||
		information.State != MEM_COMMIT)
		return false;
	const DWORD protection = information.Protect & 0xFF;
	return protection == PAGE_EXECUTE ||
		protection == PAGE_EXECUTE_READ ||
		protection == PAGE_EXECUTE_READWRITE ||
		protection == PAGE_EXECUTE_WRITECOPY;
}

bool IsReadableRange(const void* address, SIZE_T size) noexcept {
	if (!address || !size)
		return false;
	MEMORY_BASIC_INFORMATION information = {};
	if (!VirtualQuery(address, &information, sizeof(information)) ||
		information.State != MEM_COMMIT ||
		(information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		return false;
	const UINT_PTR start = reinterpret_cast<UINT_PTR>(address);
	const UINT_PTR regionEnd = reinterpret_cast<UINT_PTR>(
		information.BaseAddress) + information.RegionSize;
	return start <= regionEnd && size <= regionEnd - start;
}

} // namespace dwm_overlay
