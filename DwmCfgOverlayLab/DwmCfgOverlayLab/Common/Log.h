#pragma once

#include <Windows.h>
#include <cstdarg>
#include <cstdio>

namespace dwm_overlay {

inline void Log(const char* message) noexcept {
	char line[640] = {};
	_snprintf_s(line, _countof(line), _TRUNCATE,
		"[DwmCfgOverlayLab] %s\n", message ? message : "");
	// Keep the complete record in one debug event. DebugView treats every
	// OutputDebugString call as an independent row.
	OutputDebugStringA(line);
}

inline void LogFormat(const char* format, ...) noexcept {
	char message[512] = {};
	va_list arguments;
	va_start(arguments, format);
	_vsnprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
	va_end(arguments);
	Log(message);
}

} // namespace dwm_overlay

#define DWM_LOG(message) ::dwm_overlay::Log(message)
#define DWM_LOG_FORMAT(...) ::dwm_overlay::LogFormat(__VA_ARGS__)
#define DWM_JOIN_INNER(a, b) a##b
#define DWM_JOIN(a, b) DWM_JOIN_INNER(a, b)
#define DWM_LOG_ONCE(message) do { \
	static LONG DWM_JOIN(loggedAtLine, __LINE__) = 0; \
	if (InterlockedCompareExchange(&DWM_JOIN(loggedAtLine, __LINE__), 1, 0) == 0) \
		DWM_LOG(message); \
} while (0)
