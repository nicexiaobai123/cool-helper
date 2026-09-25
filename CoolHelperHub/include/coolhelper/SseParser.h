#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace coolhelper {

class SseParser final {
public:
	using EventCallback = std::function<void(std::string_view)>;

	explicit SseParser(EventCallback callback);
	void Feed(std::string_view bytes);
	void Finish();

private:
	void ProcessLine(std::string_view line);
	void Dispatch();

	EventCallback callback_;
	std::string pending_;
	std::string eventData_;
};

} // namespace coolhelper

