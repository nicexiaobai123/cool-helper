#pragma once

#include "../Hooks/HookTypes.h"
#include "DDisplaySurfaceAdapter.h"

namespace dwm_overlay {

class OverlayRenderer;

class FrameRouter final {
public:
	explicit FrameRouter(OverlayRenderer& renderer) noexcept
		: renderer_(renderer) {}
	void Route(const HookInvocation& invocation) noexcept;

private:
	OverlayRenderer& renderer_;
	DDisplaySurfaceAdapter displayAdapter_;
};

} // namespace dwm_overlay
