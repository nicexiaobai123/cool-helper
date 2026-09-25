#include "coolhelper/DllInjector.h"

#include "coolhelper/Logger.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// _WIN32_WINNT=0x0A00 makes psapi.h map to the K32* kernel32 exports directly;
// no psapi.lib dependency is needed.

namespace coolhelper {
namespace {

constexpr DWORD kProcessAccessForInjection =
	PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
	PROCESS_VM_WRITE | PROCESS_VM_READ;

std::string LastErrorText(const char* action) {
	return std::string(action) + "失败（Windows 错误码 " +
		std::to_string(GetLastError()) + "）";
}

std::wstring FileNameOf(const std::wstring& path) noexcept {
	std::filesystem::path filesystemPath(path);
	return filesystemPath.filename().wstring();
}

std::string Utf8FromWide(const std::wstring& text) noexcept {
	if (text.empty())
		return {};
	const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(),
		static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0)
		return {};
	std::string result(static_cast<std::size_t>(required), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		result.data(), required, nullptr, nullptr);
	return result;
}

// LoadLibraryW inside dwm.exe resolves the file as the per-session DWM-x
// virtual account, which cannot read user profiles, mapped drives, or VM
// shared folders. Impersonate the DWM token and probe the path exactly the
// way the remote loader would. This is an advisory pre-check: when the token
// cannot be opened on some systems, the probe is skipped and injection
// proceeds anyway.
bool VerifyTargetAccountCanReadDll(
	DWORD processId, const std::wstring& dllPath, bool* probed,
	std::string& error) noexcept {
	*probed = false;
	const HANDLE process = OpenProcess(
		PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (!process)
		return true; // skip; the injection itself reports access problems
	HANDLE token = nullptr;
	if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY, &token)) {
		CloseHandle(process);
		Logger::Write(LogLevel::Info,
			"DWM token probe unavailable; skipping the file access pre-check");
		return true;
	}
	HANDLE impersonationToken = nullptr;
	const BOOL duplicated = DuplicateTokenEx(token,
		TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation,
		TokenImpersonation, &impersonationToken);
	CloseHandle(token);
	if (!duplicated) {
		CloseHandle(process);
		Logger::Write(LogLevel::Info,
			"DWM token probe unavailable; skipping the file access pre-check");
		return true;
	}

	const bool impersonated = SetThreadToken(nullptr, impersonationToken) != FALSE;
	const HANDLE probe = impersonated
		? CreateFileW(dllPath.c_str(), GENERIC_READ | GENERIC_EXECUTE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
		: INVALID_HANDLE_VALUE;
	const DWORD probeError = GetLastError();
	SetThreadToken(nullptr, nullptr);
	CloseHandle(impersonationToken);
	CloseHandle(process);

	if (!impersonated) {
		Logger::Write(LogLevel::Info,
			"DWM token probe unavailable; skipping the file access pre-check");
		return true;
	}
	if (probe != INVALID_HANDLE_VALUE) {
		CloseHandle(probe);
		*probed = true;
		return true;
	}
	*probed = true;
	error = "DWM 账户（DWM-x）无法读取该 DLL 路径（Windows 错误码 " +
		std::to_string(probeError) + "），dwm 里的 LoadLibrary 必然失败。"
		"请把 DLL 移到虚拟机本地磁盘的普通目录（建议与 CoolHelperHub.exe 同目录），"
		"不要放在桌面/用户目录、映射驱动器或虚拟机共享文件夹；"
		"也可用管理员执行：icacls \"" + Utf8FromWide(dllPath) + "\" /grant Users:RX";
	Logger::Write(LogLevel::Error, "DWM account cannot read the DLL path");
	return false;
}

// Parses the DLL's PE import table and verifies that every dependency can be
// resolved on this machine (as a data file, so nothing is executed). Turns a
// bare "LoadLibraryW returned 0" into a precise missing-module report.
bool VerifyDllDependencies(
	const std::wstring& dllPath, std::string& error) {
	HANDLE file = CreateFileW(dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		error = LastErrorText("读取 DLL 文件");
		return false;
	}
	const DWORD fileSize = GetFileSize(file, nullptr);
	std::vector<BYTE> image;
	image.resize(fileSize);
	DWORD read = 0;
	const BOOL readOk =
		ReadFile(file, image.data(), fileSize, &read, nullptr);
	CloseHandle(file);
	if (!readOk || read != fileSize ||
		fileSize < sizeof(IMAGE_DOS_HEADER)) {
		error = "无法完整读取 DLL 文件，请重新复制";
		return false;
	}

	const auto* dos =
		reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
		static_cast<SIZE_T>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) >
			image.size()) {
		error = "DLL 不是有效的 PE 映像，请确认复制的是编译产物";
		return false;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
		image.data() + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		error = "DLL 不是有效的 PE 映像，请确认复制的是编译产物";
		return false;
	}
	if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
		error = "DLL 不是 x64 映像，dwm.exe 只能加载 x64 DLL";
		return false;
	}

	const auto* sections = IMAGE_FIRST_SECTION(nt);
	const WORD sectionCount = nt->FileHeader.NumberOfSections;
	const auto rvaToOffset = [&](DWORD rva) -> SIZE_T {
		for (WORD index = 0; index < sectionCount; ++index) {
			const IMAGE_SECTION_HEADER& sectionHeader = sections[index];
			const DWORD virtualSize = (std::max)(sectionHeader.Misc.VirtualSize,
				sectionHeader.SizeOfRawData);
			if (rva >= sectionHeader.VirtualAddress &&
				rva < sectionHeader.VirtualAddress + virtualSize) {
				return sectionHeader.PointerToRawData +
					(rva - sectionHeader.VirtualAddress);
			}
		}
		return 0;
	};

	const IMAGE_DATA_DIRECTORY& importDirectory =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (importDirectory.VirtualAddress == 0)
		return true;
	for (SIZE_T offset = rvaToOffset(importDirectory.VirtualAddress);
		offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= image.size();
		offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
		const auto& descriptor =
			*reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
				image.data() + offset);
		if (descriptor.Name == 0)
			break;
		const SIZE_T nameOffset = rvaToOffset(descriptor.Name);
		if (nameOffset == 0 || nameOffset >= image.size())
			continue;
		const char* name =
			reinterpret_cast<const char*>(image.data() + nameOffset);
		const std::string dependency(
			name, strnlen(name, image.size() - nameOffset));
		std::wstring wideDependency(dependency.begin(), dependency.end());
		const HMODULE module = LoadLibraryExW(wideDependency.c_str(), nullptr,
			LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		if (module) {
			FreeLibrary(module);
			continue;
		}
		error = "缺少依赖：" + dependency +
			"（本机无法解析该 DLL；若是精简版系统，"
			"可从正常系统复制到 System32 后重试）";
		return false;
	}
	return true;
}

// Returns the remote base address of the module whose file name matches, or
// nullptr. EnumProcessModulesEx needs PROCESS_QUERY_INFORMATION and
// PROCESS_VM_READ, which dwm.exe grants to an elevated caller holding
// SeDebugPrivilege.
HMODULE FindRemoteModule(
	HANDLE process, const std::wstring& dllFileName) noexcept {
	HMODULE modules[1024] = {};
	DWORD needed = 0;
	if (!EnumProcessModulesEx(process, modules, sizeof(modules), &needed,
		LIST_MODULES_ALL)) {
		return nullptr;
	}
	const DWORD count = needed / sizeof(HMODULE);
	if (count == 0 || count > 1024)
		return nullptr;
	for (DWORD index = 0; index < count; ++index) {
		wchar_t moduleName[MAX_PATH] = {};
		if (GetModuleFileNameExW(process, modules[index],
			moduleName, MAX_PATH) == 0)
			continue;
		if (_wcsicmp(FileNameOf(moduleName).c_str(), dllFileName.c_str()) == 0)
			return modules[index];
	}
	return nullptr;
}

HMODULE WaitForRemoteModule(
	HANDLE process, const std::wstring& dllFileName, DWORD timeoutMs) noexcept {
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	for (;;) {
		if (const HMODULE base = FindRemoteModule(process, dllFileName))
			return base;
		if (GetTickCount64() >= deadline)
			return nullptr;
		Sleep(100);
	}
}

// Returns true once the module has disappeared, or false on timeout.
bool WaitForRemoteModuleAbsent(
	HANDLE process, const std::wstring& dllFileName, DWORD timeoutMs) noexcept {
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	for (;;) {
		if (!FindRemoteModule(process, dllFileName))
			return true;
		if (GetTickCount64() >= deadline)
			return false;
		Sleep(100);
	}
}

} // namespace

bool EnableDebugPrivilege(std::string& error) noexcept {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(),
		TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
		error = LastErrorText("打开进程令牌");
		return false;
	}
	TOKEN_PRIVILEGES privileges = {};
	privileges.PrivilegeCount = 1;
	privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME,
		&privileges.Privileges[0].Luid)) {
		CloseHandle(token);
		error = LastErrorText("查询 SeDebugPrivilege");
		return false;
	}
	if (!AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges),
		nullptr, nullptr)) {
		CloseHandle(token);
		error = LastErrorText("启用 SeDebugPrivilege");
		return false;
	}
	const bool adjusted = GetLastError() != ERROR_NOT_ALL_ASSIGNED;
	CloseHandle(token);
	if (!adjusted) {
		error = "SeDebugPrivilege 未被授予，请以管理员身份运行";
		return false;
	}
	return true;
}

bool FindDwmProcess(DwmProcessInfo* info, std::string& error) noexcept {
	if (!info)
		return false;
	const DWORD consoleSession = WTSGetActiveConsoleSessionId();
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		error = LastErrorText("枚举系统进程");
		return false;
	}
	PROCESSENTRY32W entry = { sizeof(entry) };
	DWORD fallbackPid = 0;
	DWORD fallbackSession = 0;
	bool found = false;
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (_wcsicmp(entry.szExeFile, L"dwm.exe") != 0)
				continue;
			DWORD sessionId = 0;
			if (!ProcessIdToSessionId(entry.th32ProcessID, &sessionId))
				sessionId = 0;
			if (sessionId == consoleSession) {
				info->processId = entry.th32ProcessID;
				info->sessionId = sessionId;
				found = true;
				break;
			}
			if (!fallbackPid) {
				fallbackPid = entry.th32ProcessID;
				fallbackSession = sessionId;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	if (!found && fallbackPid) {
		info->processId = fallbackPid;
		info->sessionId = fallbackSession;
		found = true;
	}
	if (!found)
		error = "未找到 dwm.exe 进程";
	return found;
}

bool IsDllLoadedInProcess(
	DWORD processId, const std::wstring& dllPath, bool* loaded,
	std::string& error) noexcept {
	if (!loaded)
		return false;
	*loaded = false;
	const HANDLE process = OpenProcess(
		PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
	if (!process) {
		error = LastErrorText("打开 dwm 进程查询模块");
		return false;
	}
	*loaded = FindRemoteModule(process, FileNameOf(dllPath)) != nullptr;
	CloseHandle(process);
	return true;
}

// When the remote LoadLibraryW fails, load the same absolute path inertly in
// our own process (the DLL's DllMain no-ops under DWM_CFG_OVERLAY_PROBE=1) to
// capture the loader's real error code and separate file/dependency problems
// from DWM-account context problems.
std::string BuildRemoteLoadFailureMessage(const std::wstring& dllPath) {
	SetEnvironmentVariableW(L"DWM_CFG_OVERLAY_PROBE", L"1");
	const HMODULE local = LoadLibraryW(dllPath.c_str());
	const DWORD localError = local ? ERROR_SUCCESS : GetLastError();
	if (local)
		FreeLibrary(local);
	SetEnvironmentVariableW(L"DWM_CFG_OVERLAY_PROBE", nullptr);

	std::string probe;
	if (local) {
		probe = "诊断：DLL 在中台程序内以同一绝对路径加载成功，文件和依赖本身没问题。"
			"dwm 中失败只剩两种原因：DWM 账户（DWM-x）的读取上下文——"
			"请确认 DLL 位于本地磁盘且 Users 可读的目录"
			"（例如 C:\\ProgramData\\CoolHelperHub，"
			"不要放在虚拟机共享文件夹、映射驱动器或用户目录）；"
			"或防护软件针对本程序的拦截。";
	}
	else {
		probe = "诊断：本机直接 LoadLibrary 同样失败，Windows 错误码 " +
			std::to_string(localError) + "：";
		switch (localError) {
		case ERROR_MOD_NOT_FOUND:
			probe += "找不到模块或其依赖";
			break;
		case ERROR_ACCESS_DENIED:
			probe += "拒绝访问";
			break;
		case ERROR_BAD_EXE_FORMAT:
			probe += "不是有效的 x64 映像";
			break;
		case ERROR_DLL_INIT_FAILED:
			probe += "DLL 初始化失败";
			break;
		default:
			probe += "含义见 Windows 系统错误码表";
			break;
		}
	}
	return "LoadLibraryW 在 dwm 进程中返回 0。" + probe;
}

// Runs the full remote load against an open dwm process handle. Returns false
// with a diagnostic message when LoadLibraryW returns 0 or the module does not
// become resident.
bool TryRemoteLoad(
	HANDLE process, const std::wstring& dllPath, std::string& error) noexcept {
	const SIZE_T pathBytes = (wcslen(dllPath.c_str()) + 1) * sizeof(wchar_t);
	const LPVOID remotePath = VirtualAllocEx(process, nullptr, pathBytes,
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remotePath) {
		error = LastErrorText("在 dwm 进程分配内存");
		return false;
	}
	if (!WriteProcessMemory(process, remotePath, dllPath.c_str(),
		pathBytes, nullptr)) {
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		error = LastErrorText("写入 DLL 路径");
		return false;
	}

	const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	const auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		GetProcAddress(kernel32, "LoadLibraryW"));
	if (!loadLibraryW) {
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		error = "无法解析 kernel32!LoadLibraryW";
		return false;
	}

	const HANDLE thread = CreateRemoteThread(process, nullptr, 0,
		loadLibraryW, remotePath, 0, nullptr);
	if (!thread) {
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		error = LastErrorText("CreateRemoteThread");
		return false;
	}
	WaitForSingleObject(thread, 15000);
	DWORD exitCode = 0;
	GetExitCodeThread(thread, &exitCode);
	CloseHandle(thread);
	VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

	if (exitCode == 0) {
		error = BuildRemoteLoadFailureMessage(dllPath);
		Logger::Write(LogLevel::Error, "Remote LoadLibraryW failed");
		return false;
	}

	const HMODULE base = WaitForRemoteModule(
		process, FileNameOf(dllPath), 5000);
	if (!base) {
		error = "DLL 已触发加载，但未在 dwm 模块列表中确认";
		Logger::Write(LogLevel::Error, "Injected DLL not found in remote modules");
		return false;
	}
	return true;
}

bool InjectDllIntoProcess(
	DWORD processId, const std::wstring& dllPath, std::string& error) noexcept {
	std::string dependencyError;
	if (!VerifyDllDependencies(dllPath, dependencyError)) {
		error = dependencyError;
		Logger::Write(LogLevel::Error, "DLL dependency verification failed");
		return false;
	}
	std::string accessError;
	bool probed = false;
	if (!VerifyTargetAccountCanReadDll(
		processId, dllPath, &probed, accessError)) {
		error = accessError;
		return false;
	}
	Logger::Write(LogLevel::Info,
		("Injecting DLL: " + Utf8FromWide(dllPath)).c_str());

	const HANDLE process = OpenProcess(
		kProcessAccessForInjection, FALSE, processId);
	if (!process) {
		error = LastErrorText("打开 dwm 进程（需要管理员权限）");
		return false;
	}
	if (TryRemoteLoad(process, dllPath, error)) {
		CloseHandle(process);
		return true;
	}
	CloseHandle(process);
	return false;
}

bool EjectDllFromProcess(
	DWORD processId, const std::wstring& dllPath, std::string& error) noexcept {
	const HANDLE process = OpenProcess(kProcessAccessForInjection, FALSE, processId);
	if (!process) {
		error = LastErrorText("打开 dwm 进程（需要管理员权限）");
		return false;
	}

	const HMODULE remoteBase = FindRemoteModule(process, FileNameOf(dllPath));
	if (!remoteBase) {
		CloseHandle(process);
		error = "dwm 进程中未找到该 DLL";
		return false;
	}

	// Map our own copy without initializing it to resolve the export RVA.
	const HMODULE local = LoadLibraryExW(
		dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
	if (!local) {
		CloseHandle(process);
		error = LastErrorText("读取 DLL 导出表");
		return false;
	}
	const FARPROC shutdownExport = GetProcAddress(local, "ShutdownDwmOverlay");
	if (!shutdownExport) {
		FreeLibrary(local);
		CloseHandle(process);
		error = "DLL 缺少 ShutdownDwmOverlay 导出";
		return false;
	}
	const uintptr_t shutdownRva =
		reinterpret_cast<uintptr_t>(shutdownExport) - reinterpret_cast<uintptr_t>(local);
	FreeLibrary(local);

	// Let the runtime restore hooks and drain workers before FreeLibrary.
	const auto remoteShutdown = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		reinterpret_cast<uintptr_t>(remoteBase) + shutdownRva);
	const HANDLE shutdownThread = CreateRemoteThread(process, nullptr, 0,
		remoteShutdown, nullptr, 0, nullptr);
	if (!shutdownThread) {
		CloseHandle(process);
		error = LastErrorText("CreateRemoteThread(ShutdownDwmOverlay)");
		return false;
	}
	WaitForSingleObject(shutdownThread, 20000);
	DWORD shutdownResult = 0;
	GetExitCodeThread(shutdownThread, &shutdownResult);
	CloseHandle(shutdownThread);

	if (shutdownResult == STILL_ACTIVE) {
		CloseHandle(process);
		error = "ShutdownDwmOverlay 20 秒内未返回，已放弃卸载以保持 dwm 稳定";
		Logger::Write(LogLevel::Error, "Remote shutdown timed out");
		return false;
	}
	if (shutdownResult == 0) {
		CloseHandle(process);
		error = "运行时无法安全恢复钩子（ShutdownDwmOverlay 返回 FALSE），"
			"DLL 将保留加载以保持 dwm 稳定";
		Logger::Write(LogLevel::Error, "Remote ShutdownDwmOverlay returned FALSE");
		return false;
	}

	const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	const auto freeLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		GetProcAddress(kernel32, "FreeLibrary"));
	if (!freeLibrary) {
		CloseHandle(process);
		error = "无法解析 kernel32!FreeLibrary";
		return false;
	}
	const HANDLE freeThread = CreateRemoteThread(process, nullptr, 0,
		freeLibrary, static_cast<LPVOID>(const_cast<HMODULE>(remoteBase)),
		0, nullptr);
	if (!freeThread) {
		CloseHandle(process);
		error = LastErrorText("CreateRemoteThread(FreeLibrary)");
		return false;
	}
	WaitForSingleObject(freeThread, 10000);
	CloseHandle(freeThread);
	CloseHandle(process);

	// Reopen with query rights and confirm the loader released the module.
	const HANDLE verifyProcess = OpenProcess(
		PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
	bool stillLoaded = false;
	if (verifyProcess) {
		stillLoaded = !WaitForRemoteModuleAbsent(
			verifyProcess, FileNameOf(dllPath), 3000);
		CloseHandle(verifyProcess);
	}
	else {
		stillLoaded = true; // Without query access we cannot confirm safety.
	}
	if (stillLoaded) {
		error = "FreeLibrary 已执行，但 DLL 仍残留在 dwm 模块列表中";
		Logger::Write(LogLevel::Error, "DLL still resident after FreeLibrary");
		return false;
	}
	return true;
}

} // namespace coolhelper
