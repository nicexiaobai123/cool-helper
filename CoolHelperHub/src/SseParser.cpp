#include "coolhelper/SseParser.h"

#include <utility>

namespace coolhelper {

SseParser::SseParser(EventCallback callback)
	: callback_(std::move(callback)) {}

void SseParser::Feed(std::string_view bytes) {
	pending_.append(bytes);
	for (;;) {
		const auto newline = pending_.find('\n');
		if (newline == std::string::npos)
			break;
		std::string line = pending_.substr(0, newline);
		pending_.erase(0, newline + 1);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		ProcessLine(line);
	}
}

void SseParser::Finish() {
	if (!pending_.empty()) {
		if (pending_.back() == '\r')
			pending_.pop_back();
		ProcessLine(pending_);
		pending_.clear();
	}
	Dispatch();
}

void SseParser::ProcessLine(std::string_view line) {
	if (line.empty()) {
		Dispatch();
		return;
	}
	if (line.front() == ':')
		return;
	if (!line.starts_with("data:"))
		return;
	line.remove_prefix(5);
	if (!line.empty() && line.front() == ' ')
		line.remove_prefix(1);
	if (!eventData_.empty())
		eventData_.push_back('\n');
	eventData_.append(line);
}

void SseParser::Dispatch() {
	if (eventData_.empty())
		return;
	callback_(eventData_);
	eventData_.clear();
}

} // namespace coolhelper

