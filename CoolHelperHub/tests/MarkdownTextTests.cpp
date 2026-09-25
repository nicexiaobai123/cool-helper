#include "../../Shared/MarkdownText.h"

#include <cstdlib>
#include <iostream>
#include <string>

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
	return 0;
}
