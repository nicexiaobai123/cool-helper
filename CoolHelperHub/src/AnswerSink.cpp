#include "coolhelper/AnswerSink.h"

namespace coolhelper {

void QueuedAnswerSink::SetWindow(HWND window) noexcept {
	std::scoped_lock lock(mutex_);
	window_ = window;
}

void QueuedAnswerSink::Publish(const AnswerEvent& event) noexcept {
	HWND window = nullptr;
	try {
		std::scoped_lock lock(mutex_);
		events_.push_back(event);
		window = window_;
	}
	catch (...) {
		return;
	}
	if (window)
		PostMessageW(window, kAnswerEventMessage, 0, 0);
}

std::vector<AnswerEvent> QueuedAnswerSink::Drain() noexcept {
	std::vector<AnswerEvent> result;
	try {
		std::scoped_lock lock(mutex_);
		result.reserve(events_.size());
		while (!events_.empty()) {
			result.push_back(std::move(events_.front()));
			events_.pop_front();
		}
	}
	catch (...) {
		result.clear();
	}
	return result;
}

void TeeAnswerSink::Add(IAnswerSink* sink) noexcept {
	if (!sink || sinkCount_ >= kMaxSinks)
		return;
	sinks_[sinkCount_++] = sink;
}

void TeeAnswerSink::Publish(const AnswerEvent& event) noexcept {
	for (int index = 0; index < sinkCount_; ++index) {
		if (sinks_[index])
			sinks_[index]->Publish(event);
	}
}

} // namespace coolhelper

