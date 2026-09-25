#pragma once

namespace coolhelper {

enum class OverlayControlCommand {
	Show,
	Hide,
	Shutdown
};

class IOverlayController {
public:
	virtual ~IOverlayController() = default;
	virtual bool Send(OverlayControlCommand command) noexcept = 0;
};

} // namespace coolhelper

