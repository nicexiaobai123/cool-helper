#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace coolhelper_shared {
namespace detail {

struct MathReplacement {
	std::string_view command;
	std::string_view text;
};

inline std::string_view MathCommandReplacement(
	std::string_view command) noexcept {
	static constexpr MathReplacement replacements[] = {
		{ "cdot", "×" }, { "times", "×" }, { "div", "÷" },
		{ "pm", "±" }, { "le", "≤" }, { "leq", "≤" },
		{ "ge", "≥" }, { "geq", "≥" }, { "ne", "≠" },
		{ "neq", "≠" }, { "approx", "≈" }, { "equiv", "≡" },
		{ "to", "→" }, { "rightarrow", "→" },
		{ "leftarrow", "←" }, { "Rightarrow", "⇒" },
		{ "infty", "∞" }, { "sum", "∑" }, { "prod", "∏" },
		{ "sqrt", "√" }, { "log", "log" }, { "ln", "ln" },
		{ "min", "min" }, { "max", "max" },
		{ "Theta", "Θ" }, { "Omega", "Ω" },
		{ "alpha", "α" }, { "beta", "β" }, { "gamma", "γ" },
		{ "delta", "δ" }, { "lambda", "λ" }, { "mu", "μ" },
		{ "pi", "π" }, { "sigma", "σ" },
		{ "quad", " " }, { "qquad", "  " },
		{ "left", "" }, { "right", "" }, { "text", "" },
		{ "mathrm", "" }, { "mathbf", "" }, { "operatorname", "" }
	};
	for (const auto& replacement : replacements) {
		if (replacement.command == command)
			return replacement.text;
	}
	return {};
}

inline bool IsAsciiLetter(char value) noexcept {
	return (value >= 'a' && value <= 'z') ||
		(value >= 'A' && value <= 'Z');
}

inline bool IsFormattingCommand(std::string_view command) noexcept {
	return command == "left" || command == "right" || command == "text" ||
		command == "mathrm" || command == "mathbf" ||
		command == "operatorname";
}

inline bool FindClosingDelimiter(std::string_view input, std::size_t begin,
	std::string_view delimiter, std::size_t& closing) noexcept {
	for (std::size_t i = begin; i + delimiter.size() <= input.size(); ++i) {
		if (input[i] == '\n' || input[i] == '\r')
			return false;
		if (input.compare(i, delimiter.size(), delimiter) != 0)
			continue;
		std::size_t slashes = 0;
		for (std::size_t p = i; p > 0 && input[p - 1] == '\\'; --p)
			++slashes;
		if ((slashes & 1u) == 0u) {
			closing = i;
			return true;
		}
	}
	return false;
}

inline void NormalizeMathContent(std::string_view math, std::string& output);

inline bool NormalizeBracedGroup(std::string_view math, std::size_t& position,
	std::string& output) {
	if (position >= math.size() || math[position] != '{')
		return false;
	const std::size_t contentBegin = ++position;
	int depth = 1;
	while (position < math.size() && depth > 0) {
		if (math[position] == '{')
			++depth;
		else if (math[position] == '}')
			--depth;
		if (depth > 0)
			++position;
	}
	if (depth != 0)
		return false;
	NormalizeMathContent(
		math.substr(contentBegin, position - contentBegin), output);
	++position;
	return true;
}

inline void NormalizeMathContent(std::string_view math, std::string& output) {
	for (std::size_t i = 0; i < math.size();) {
		const char value = math[i];
		if (value == '{' || value == '}') {
			++i;
			continue;
		}
		if (value == '~') {
			output.push_back(' ');
			++i;
			continue;
		}
		if (value != '\\') {
			output.push_back(value);
			++i;
			continue;
		}

		++i;
		while (i < math.size() && math[i] == '\\')
			++i;
		if (i >= math.size())
			break;
		if (!IsAsciiLetter(math[i])) {
			if (math[i] == ',' || math[i] == ';' || math[i] == ' ')
				output.push_back(' ');
			else if (math[i] != '!')
				output.push_back(math[i]);
			++i;
			continue;
		}

		const std::size_t commandBegin = i;
		while (i < math.size() && IsAsciiLetter(math[i]))
			++i;
		const std::string_view command = math.substr(commandBegin,
			i - commandBegin);
		if (command == "frac") {
			const std::size_t savedPosition = i;
			std::string numerator;
			std::string denominator;
			if (NormalizeBracedGroup(math, i, numerator) &&
				NormalizeBracedGroup(math, i, denominator)) {
				const bool wrapNumerator = numerator.find_first_of(" +-*/") !=
					std::string::npos;
				const bool wrapDenominator = denominator.find_first_of(" +-*/") !=
					std::string::npos;
				if (wrapNumerator) output.push_back('(');
				output.append(numerator);
				if (wrapNumerator) output.push_back(')');
				output.push_back('/');
				if (wrapDenominator) output.push_back('(');
				output.append(denominator);
				if (wrapDenominator) output.push_back(')');
				continue;
			}
			i = savedPosition;
			output.append("frac");
			continue;
		}
		const std::string_view replacement = MathCommandReplacement(command);
		if (!replacement.empty())
			output.append(replacement.data(), replacement.size());
		else if (!IsFormattingCommand(command))
			output.append(command.data(), command.size());
	}
}

} // namespace detail

// Converts the small LaTeX subset commonly returned by chat models into
// readable plain UTF-8 while preserving Markdown code spans and fenced blocks.
inline void NormalizeMarkdownMath(const char* text, std::string& output) {
	output.clear();
	if (!text)
		return;
	const std::string_view input(text);
	output.reserve(input.size());
	bool fencedCode = false;
	bool inlineCode = false;

	for (std::size_t i = 0; i < input.size();) {
		if (i + 3 <= input.size() && input.compare(i, 3, "```") == 0) {
			fencedCode = !fencedCode;
			output.append("```");
			i += 3;
			continue;
		}
		if (!fencedCode && input[i] == '`') {
			inlineCode = !inlineCode;
			output.push_back(input[i++]);
			continue;
		}
		if (fencedCode || inlineCode) {
			output.push_back(input[i++]);
			continue;
		}
		if (input[i] == '\\' && i + 1 < input.size() && input[i + 1] == '$') {
			output.push_back('$');
			i += 2;
			continue;
		}

		std::string_view opening;
		std::string_view closingDelimiter;
		if (input[i] == '$') {
			opening = (i + 1 < input.size() && input[i + 1] == '$')
				? std::string_view("$$") : std::string_view("$");
			closingDelimiter = opening;
		}
		else if (i + 1 < input.size() && input[i] == '\\' &&
			(input[i + 1] == '(' || input[i + 1] == '[')) {
			opening = input[i + 1] == '(' ? std::string_view("\\(")
				: std::string_view("\\[");
			closingDelimiter = input[i + 1] == '(' ? std::string_view("\\)")
				: std::string_view("\\]");
		}

		if (!opening.empty()) {
			const std::size_t contentBegin = i + opening.size();
			std::size_t closing = 0;
			const bool validOpening = opening.size() == 2 ||
				(contentBegin < input.size() && input[contentBegin] != ' ' &&
				 input[contentBegin] != '\t');
			if (validOpening && detail::FindClosingDelimiter(input,
				contentBegin, closingDelimiter, closing) &&
				(closing == contentBegin || (input[closing - 1] != ' ' &&
				 input[closing - 1] != '\t'))) {
				detail::NormalizeMathContent(
					input.substr(contentBegin, closing - contentBegin), output);
				i = closing + closingDelimiter.size();
				continue;
			}
		}

		output.push_back(input[i++]);
	}
}

} // namespace coolhelper_shared
