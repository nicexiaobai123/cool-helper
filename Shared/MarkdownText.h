#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace coolhelper_shared {

enum MarkdownInlineStyle : int {
	MarkdownPlain = 0,
	MarkdownBold = 1,
	MarkdownCode = 2,
	MarkdownLinkText = 3,
	MarkdownLinkUrl = 4
};

struct MarkdownInlineRun {
	const char* begin;
	const char* end;
	int style;
};

inline const char* MarkdownFindChar(
	const char* begin, const char* end, char value) noexcept {
	const void* found = std::memchr(
		begin, value, static_cast<std::size_t>(end - begin));
	return static_cast<const char*>(found);
}

inline const char* MarkdownFindDoubleChar(
	const char* begin, const char* end, char value) noexcept {
	for (const char* scan = begin; scan + 1 < end; ++scan) {
		if (scan[0] == value && scan[1] == value)
			return scan;
	}
	return nullptr;
}

inline const char* MarkdownSkipSpaces(
	const char* position, const char* end) noexcept {
	while (position < end && (*position == ' ' || *position == '\t'))
		++position;
	return position;
}

inline bool MarkdownIsBlankLine(
	const char* begin, const char* end) noexcept {
	for (const char* position = begin; position < end; ++position) {
		if (*position != ' ' && *position != '\t')
			return false;
	}
	return true;
}

inline bool MarkdownIsHorizontalRule(
	const char* begin, const char* end) noexcept {
	if (begin >= end)
		return false;
	for (const char* position = begin; position < end; ++position) {
		if (*position != begin[0] && *position != ' ')
			return false;
	}
	return true;
}

inline bool MarkdownParseOrderedListMarker(
	const char* position, const char* end,
	const char** contentBegin) noexcept {
	if (position >= end || *position < '0' || *position > '9')
		return false;
	const char* markerEnd = position;
	int digits = 0;
	while (markerEnd < end && *markerEnd >= '0' && *markerEnd <= '9' &&
		digits < 4) {
		++markerEnd;
		++digits;
	}
	if (markerEnd + 1 < end &&
		(*markerEnd == '.' || *markerEnd == ')') && markerEnd[1] == ' ') {
		*contentBegin = markerEnd + 2;
		return true;
	}
	return false;
}

inline bool MarkdownIsTableSeparatorCell(const std::string& cell) noexcept {
	std::size_t index = 0;
	const std::size_t size = cell.size();
	while (index < size && (cell[index] == ' ' || cell[index] == '\t'))
		++index;
	if (index < size && cell[index] == ':')
		++index;
	std::size_t dashes = 0;
	while (index < size && cell[index] == '-') {
		++index;
		++dashes;
	}
	if (index < size && cell[index] == ':')
		++index;
	while (index < size && (cell[index] == ' ' || cell[index] == '\t'))
		++index;
	return dashes >= 2 && index == size;
}

inline bool MarkdownParseTableRow(
	const char* begin, const char* end,
	std::vector<std::string>* cells) {
	cells->clear();
	const char* position = begin + 1; // caller guarantees begin[0] == '|'
	std::string cell;
	while (position <= end) {
		if (position == end || *position == '|') {
			std::size_t lead = 0;
			std::size_t trail = cell.size();
			while (lead < trail &&
				(cell[lead] == ' ' || cell[lead] == '\t'))
				++lead;
			while (trail > lead &&
				(cell[trail - 1] == ' ' || cell[trail - 1] == '\t'))
				--trail;
			cells->emplace_back(cell, lead, trail - lead);
			cell.clear();
			if (position == end)
				break;
			++position;
			continue;
		}
		cell.push_back(*position++);
	}
	if (!cells->empty() && cells->back().empty())
		cells->pop_back();
	return !cells->empty();
}

inline void CollectMarkdownInlineRuns(
	const char* begin, const char* end,
	std::vector<MarkdownInlineRun>& runs) {
	const char* position = begin;
	while (position < end) {
		const char* doubleStar = MarkdownFindDoubleChar(position, end, '*');
		const char* backtick = MarkdownFindChar(position, end, '`');
		const char* linkStart = nullptr;
		for (const char* scan = position; scan < end;) {
			const char* bracket = MarkdownFindChar(scan, end, '[');
			if (!bracket)
				break;
			const char* closeBracket = MarkdownFindChar(bracket + 1, end, ']');
			if (closeBracket && closeBracket + 1 < end &&
				closeBracket[1] == '(' &&
				MarkdownFindChar(closeBracket + 2, end, ')')) {
				linkStart = bracket;
				break;
			}
			scan = bracket + 1;
		}
		const char* marker = nullptr;
		if (doubleStar && backtick)
			marker = doubleStar < backtick ? doubleStar : backtick;
		else
			marker = doubleStar ? doubleStar : backtick;
		if (linkStart && (!marker || linkStart < marker))
			marker = linkStart;

		if (!marker) {
			runs.push_back({position, end, MarkdownPlain});
			return;
		}
		if (marker > position)
			runs.push_back({position, marker, MarkdownPlain});
		if (marker == linkStart) {
			const char* closeBracket = MarkdownFindChar(marker + 1, end, ']');
			const char* urlClose = MarkdownFindChar(closeBracket + 2, end, ')');
			runs.push_back({marker + 1, closeBracket, MarkdownLinkText});
			runs.push_back({closeBracket + 1, urlClose + 1, MarkdownLinkUrl});
			position = urlClose + 1;
		}
		else if (*marker == '`') {
			const char* closing = MarkdownFindChar(marker + 1, end, '`');
			if (!closing) {
				runs.push_back({marker, end, MarkdownPlain});
				return;
			}
			runs.push_back({marker + 1, closing, MarkdownCode});
			position = closing + 1;
		}
		else {
			const char* closing = MarkdownFindDoubleChar(marker + 2, end, '*');
			if (!closing) {
				runs.push_back({marker, end, MarkdownPlain});
				return;
			}
			runs.push_back({marker + 2, closing, MarkdownBold});
			position = closing + 2;
		}
	}
}

inline unsigned DecodeUtf8(
	const char* text, const char* end, int* length) noexcept {
	unsigned codepoint = static_cast<unsigned char>(*text);
	*length = 1;
	if (codepoint < 0x80)
		return codepoint;
	if ((codepoint & 0xE0) == 0xC0 && text + 1 < end) {
		*length = 2;
		return ((codepoint & 0x1F) << 6) | (text[1] & 0x3F);
	}
	if ((codepoint & 0xF0) == 0xE0 && text + 2 < end) {
		*length = 3;
		return ((codepoint & 0x0F) << 12) |
			((text[1] & 0x3F) << 6) | (text[2] & 0x3F);
	}
	if ((codepoint & 0xF8) == 0xF0 && text + 3 < end) {
		*length = 4;
		return ((codepoint & 0x07) << 18) |
			((text[1] & 0x3F) << 12) |
			((text[2] & 0x3F) << 6) | (text[3] & 0x3F);
	}
	return codepoint;
}

inline bool IsWideCodepoint(unsigned codepoint) noexcept {
	return codepoint >= 0x1100 &&
		(codepoint <= 0x11FF || codepoint >= 0x2E80);
}

inline const char* MarkdownWrapUnitEnd(
	const char* unitBegin, const char* runEnd) noexcept {
	int charLength = 0;
	const unsigned codepoint = DecodeUtf8(unitBegin, runEnd, &charLength);
	const char* unitEnd = unitBegin + charLength;
	if (codepoint != ' ' && !IsWideCodepoint(codepoint)) {
		while (unitEnd < runEnd) {
			int nextLength = 0;
			const unsigned next = DecodeUtf8(unitEnd, runEnd, &nextLength);
			if (next == ' ' || IsWideCodepoint(next))
				break;
			unitEnd += nextLength;
		}
	}
	return unitEnd;
}

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
