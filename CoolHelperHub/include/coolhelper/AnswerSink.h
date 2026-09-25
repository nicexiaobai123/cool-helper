#pragma once

#include <Windows.h>

#include <deque>
#include <mutex>
#include <vector>

#include "AppTypes.h"

namespace coolhelper {

constexpr UINT kAnswerEventMessage = WM_APP + 0x21;

class IAnswerSink {
public:
	virtual ~IAnswerSink() = default;
	virtual void Publish(const AnswerEvent& event) noexcept = 0;
};

class QueuedAnswerSink final : public IAnswerSink {
public:
	void SetWindow(HWND window) noexcept;
	void Publish(const AnswerEvent& event) noexcept override;
	std::vector<AnswerEvent> Drain() noexcept;

private:
	std::mutex mutex_;
	std::deque<AnswerEvent> events_;
	HWND window_ = nullptr;
};

// Fans a single answer stream out to several sinks. Sinks are registered
// once during startup, before any request can publish.
class TeeAnswerSink final : public IAnswerSink {
public:
	void Add(IAnswerSink* sink) noexcept;
	void Publish(const AnswerEvent& event) noexcept override;

private:
	static constexpr int kMaxSinks = 4;
	IAnswerSink* sinks_[kMaxSinks] = {};
	int sinkCount_ = 0;
};

} // namespace coolhelper

