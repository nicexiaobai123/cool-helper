#include "InputSource.h"

#include <Windows.h>

namespace dwm_overlay {

void DesktopPollingInputSource::Update(ImGuiIO& io) noexcept {
	POINT desktopOrigin = {};
	ClientToScreen(GetDesktopWindow(), &desktopOrigin);
	POINT cursorPosition = {};
	GetCursorPos(&cursorPosition);
	io.MousePos = ImVec2(
		static_cast<float>(cursorPosition.x - desktopOrigin.x),
		static_cast<float>(cursorPosition.y - desktopOrigin.y));
	io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
	io.MouseDown[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
}

} // namespace dwm_overlay
