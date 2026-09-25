#include "coolhelper/SseParser.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool Expect(bool condition, const char* message) {
	if (!condition)
		std::cerr << "FAILED: " << message << '\n';
	return condition;
}

} // namespace

int main() {
	bool passed = true;
	std::vector<std::string> events;
	coolhelper::SseParser parser([&](std::string_view event) {
		events.emplace_back(event);
	});

	parser.Feed("data: {\"choices\":[{\"delta\":{");
	parser.Feed("\"content\":\"你");
	parser.Feed("好\"}}]}\r\n\r\ndata: [DO");
	parser.Feed("NE]\n\n");
	parser.Finish();

	passed &= Expect(events.size() == 2, "two fragmented SSE events are emitted");
	if (events.size() == 2) {
		passed &= Expect(events[0].find("你好") != std::string::npos,
			"UTF-8 payload survives arbitrary chunk boundaries");
		passed &= Expect(events[1] == "[DONE]", "DONE marker is preserved");
	}

	events.clear();
	coolhelper::SseParser multiline([&](std::string_view event) {
		events.emplace_back(event);
	});
	multiline.Feed(": keepalive\n");
	multiline.Feed("data: first\ndata: second\n\n");
	passed &= Expect(events.size() == 1 && events[0] == "first\nsecond",
		"multi-line data events are joined");

	return passed ? 0 : 1;
}

