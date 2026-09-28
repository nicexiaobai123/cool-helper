#include "../../Shared/MarkdownText.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Expect(const char* input, const char* expected) {
	std::string actual;
	coolhelper_shared::NormalizeMarkdownMath(input, actual);
	if (actual == expected)
		return;
	std::cerr << "Input:    " << input << "\nExpected: " << expected
		<< "\nActual:   " << actual << '\n';
	std::exit(1);
}

[[noreturn]] void Fail(const char* message) {
	std::cerr << message << '\n';
	std::exit(1);
}

void ExpectInlineRuns() {
	const std::string text = "plain **bold** `code x` [link](url)";
	std::vector<coolhelper_shared::MarkdownInlineRun> runs;
	coolhelper_shared::CollectMarkdownInlineRuns(
		text.data(), text.data() + text.size(), runs);
	const int expectedStyles[] = {
		coolhelper_shared::MarkdownPlain,
		coolhelper_shared::MarkdownBold,
		coolhelper_shared::MarkdownPlain,
		coolhelper_shared::MarkdownCode,
		coolhelper_shared::MarkdownPlain,
		coolhelper_shared::MarkdownLinkText,
		coolhelper_shared::MarkdownLinkUrl
	};
	const char* expectedText[] = {
		"plain ", "bold", " ", "code x", " ", "link", "(url)"
	};
	if (runs.size() != sizeof(expectedStyles) / sizeof(expectedStyles[0]))
		Fail("Unexpected inline run count");
	for (std::size_t index = 0; index < runs.size(); ++index) {
		if (runs[index].style != expectedStyles[index] ||
			std::string(runs[index].begin, runs[index].end) != expectedText[index])
			Fail("Unexpected inline run");
	}
}

void ExpectBlockHelpers() {
	const std::string row = "| name | value |";
	std::vector<std::string> cells;
	if (!coolhelper_shared::MarkdownParseTableRow(
		row.data(), row.data() + row.size(), &cells) ||
		cells.size() != 2 || cells[0] != "name" || cells[1] != "value")
		Fail("Table row parsing failed");
	if (!coolhelper_shared::MarkdownIsTableSeparatorCell(":---:") ||
		coolhelper_shared::MarkdownIsTableSeparatorCell("-- x"))
		Fail("Table separator parsing failed");

	const std::string ordered = "12. content";
	const char* content = nullptr;
	if (!coolhelper_shared::MarkdownParseOrderedListMarker(
		ordered.data(), ordered.data() + ordered.size(), &content) ||
		std::string(content, ordered.data() + ordered.size()) != "content")
		Fail("Ordered list parsing failed");

	const std::string utf8 = "函A";
	if (coolhelper_shared::MarkdownWrapUnitEnd(
		utf8.data(), utf8.data() + utf8.size()) != utf8.data() + 3)
		Fail("UTF-8 wrap boundary failed");
	const std::string ascii = "target - x";
	if (coolhelper_shared::MarkdownWrapUnitEnd(
		ascii.data(), ascii.data() + ascii.size()) != ascii.data() + 6)
		Fail("ASCII wrap boundary failed");
}

} // namespace

int main() {
	Expect("复杂度为 $O(N \\cdot L)$，其中 $N$ 是数量。",
		"复杂度为 O(N × L)，其中 N 是数量。");
	Expect("$O(L \\log N)$ / \\(x \\leq y\\)",
		"O(L log N) / x ≤ y");
	Expect("代码 `price = $5` 与 ```cpp\n$O(n)$\n``` 保持原样",
		"代码 `price = $5` 与 ```cpp\n$O(n)$\n``` 保持原样");
	Expect("未闭合的 $100 和转义的 \\$5", "未闭合的 $100 和转义的 $5");
	Expect("$$\\Theta(n^2) \\approx \\infty$$", "Θ(n^2) ≈ ∞");
	Expect("$\\frac{a + b}{2} \\leq \\mathrm{limit}$",
		"(a + b)/2 ≤ limit");
	ExpectInlineRuns();
	ExpectBlockHelpers();
	return 0;
}
