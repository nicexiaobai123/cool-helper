#pragma once

#include <Windows.h>
#include "../../../Shared/OverlayControlProtocol.h"

#include <cstdint>
#include <string>

namespace coolhelper {

namespace overlaycontrol = ::coolhelper_overlay;
using DisplayMode = overlaycontrol::DisplayMode;

// Low-frequency command writer. Commands are sent synchronously from the UI
// thread (a single writer by design); liveness is driven by KeepAlive() which
// the app calls once per message-loop iteration.
class OverlayControlChannel final {
public:
	OverlayControlChannel() = default;
	~OverlayControlChannel();
	OverlayControlChannel(const OverlayControlChannel&) = delete;
	OverlayControlChannel& operator=(const OverlayControlChannel&) = delete;

	bool Start(std::string& error) noexcept;
	void Stop() noexcept;
	bool IsRunning() const noexcept { return running_; }

	// Called once per app loop iteration to refresh the hub heartbeat.
	void KeepAlive() noexcept;

	// True while DwmCfgOverlayLab's control worker heartbeats the header.
	bool IsDllConnected() const noexcept;

	// -1 = unknown, 0 = hidden, 1 = visible (DLL-reported).
	int QueryOverlayVisible() const noexcept;

	bool SendSetOverlayVisible(bool visible) noexcept;
	bool SendScrollAnswer(int direction) noexcept;
    bool SetDisplayMode(DisplayMode mode) noexcept;
    // -1 = disconnected/older DLL, otherwise applied requested mode (0/1/2).
    int QueryDisplayMode() const noexcept;

private:
	static bool HeartbeatFresh(UINT64 tick) noexcept;
	bool SendCommand(UINT32 type, UINT32 value) noexcept;

	bool running_ = false;
	HANDLE readyEvent_ = nullptr;
	HANDLE mapping_ = nullptr;
	overlaycontrol::ControlSharedHeader* header_ = nullptr;
	UINT32 sequence_ = 0;
};

} // namespace coolhelper
