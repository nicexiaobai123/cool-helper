#pragma once

#include "../IMGUI/imgui.h"

namespace dwm_overlay {

class IInputSource {
public:
	virtual ~IInputSource() = default;
	virtual void Update(ImGuiIO& io) noexcept = 0;
};

// Preserves the current handleless behavior. A controller/IPC-backed source
// can later replace this class without touching the renderer.
class DesktopPollingInputSource final : public IInputSource {
public:
	void Update(ImGuiIO& io) noexcept override;
};

} // namespace dwm_overlay
