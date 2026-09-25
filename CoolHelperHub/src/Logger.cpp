#include "coolhelper/Logger.h"

#include <Windows.h>
#include <ShlObj.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace coolhelper {
namespace {

std::mutex g_logMutex;
std::ofstream g_logFile;

const char* LevelName(LogLevel level) noexcept {
	switch (level) {
	case LogLevel::Warning: return "WARN";
	case LogLevel::Error: return "ERROR";
	default: return "INFO";
	}
}

std::filesystem::path LocalDataDirectory() {
	PWSTR rawPath = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath)))
		return {};
	std::filesystem::path path(rawPath);
	CoTaskMemFree(rawPath);
	return path / L"CoolHelperHub";
}

} // namespace

void Logger::Initialize() noexcept {
	try {
		std::scoped_lock lock(g_logMutex);
		const auto directory = LocalDataDirectory() / L"logs";
		std::filesystem::create_directories(directory);
		const auto path = directory / L"CoolHelperHub.log";
		std::error_code error;
		if (std::filesystem::exists(path, error) &&
			std::filesystem::file_size(path, error) > 2 * 1024 * 1024) {
			const auto backup = directory / L"CoolHelperHub.log.1";
			std::filesystem::remove(backup, error);
			std::filesystem::rename(path, backup, error);
		}
		g_logFile.open(path, std::ios::binary | std::ios::app);
	}
	catch (...) {
	}
}

void Logger::Shutdown() noexcept {
	try {
		std::scoped_lock lock(g_logMutex);
		if (g_logFile.is_open()) {
			g_logFile.flush();
			g_logFile.close();
		}
	}
	catch (...) {
	}
}

void Logger::Write(LogLevel level, std::string_view message) noexcept {
	try {
		const auto now = std::chrono::system_clock::now();
		const std::time_t time = std::chrono::system_clock::to_time_t(now);
		std::tm local = {};
		localtime_s(&local, &time);

		std::ostringstream line;
		line << '[' << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << "] ["
			<< LevelName(level) << "] [" << GetCurrentThreadId() << "] "
			<< message << "\r\n";
		const std::string text = line.str();
		OutputDebugStringA(text.c_str());

		std::scoped_lock lock(g_logMutex);
		if (g_logFile.is_open()) {
			g_logFile.write(text.data(), static_cast<std::streamsize>(text.size()));
			g_logFile.flush();
		}
	}
	catch (...) {
	}
}

} // namespace coolhelper

