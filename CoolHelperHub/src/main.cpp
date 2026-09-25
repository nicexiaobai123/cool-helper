#include "coolhelper/App.h"

#include <Windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	const HANDLE singleton = CreateMutexW(
		nullptr, FALSE, L"Local\\CoolHelperHub.Singleton.v1");
	if (!singleton)
		return 1;
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(singleton);
		return 0;
	}

	coolhelper::App app;
	const int result = app.Run(instance, showCommand);
	CloseHandle(singleton);
	return result;
}
