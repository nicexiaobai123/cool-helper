#include "InputSource.h"

#include <Windows.h>

namespace dwm_overlay {

void DesktopPollingInputSource::Update(ImGuiIO& io) noexcept {
	POINT cursorPosition = {};
	GetCursorPos(&cursorPosition);
	io.AddMousePosEvent(
		static_cast<float>(cursorPosition.x - origin_.x),
		static_cast<float>(cursorPosition.y - origin_.y));
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
}

} // namespace dwm_overlay
