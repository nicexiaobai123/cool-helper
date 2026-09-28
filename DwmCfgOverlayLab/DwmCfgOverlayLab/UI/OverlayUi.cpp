#include "OverlayUi.h"
#include "../../../Shared/MarkdownText.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace dwm_overlay {
namespace {

// Accent palette for the dark glass look.
constexpr ImU32 kAccentColor = IM_COL32(96, 165, 250, 255);      // bullets, numbers
constexpr ImU32 kLinkColor = IM_COL32(125, 190, 255, 255);
constexpr ImU32 kHeadingColor = IM_COL32(236, 241, 250, 255);
constexpr ImU32 kHeadingUnderline = IM_COL32(96, 165, 250, 150);
constexpr ImU32 kQuoteColor = IM_COL32(158, 168, 182, 255);
constexpr ImU32 kQuoteBarColor = IM_COL32(96, 165, 250, 140);
constexpr ImU32 kInlineCodeColor = IM_COL32(165, 225, 195, 255);
constexpr ImU32 kInlineCodeBackground = IM_COL32(255, 255, 255, 22);
constexpr ImU32 kCodeBackground = IM_COL32(14, 18, 25, 235);
constexpr ImU32 kTableLine = IM_COL32(255, 255, 255, 34);
constexpr ImU32 kTableHeaderLine = IM_COL32(96, 165, 250, 170);
constexpr UINT64 kAnswerCopyCapacity = 512 * 1024 + 2;
constexpr float kOverlayMargin = 32.0f;
constexpr float kDefaultOverlayWidth = 840.0f;
constexpr float kDefaultOverlayHeight = 820.0f;

ImFont* FontOrDefault(ImFont* font) noexcept {
	return font && font->IsLoaded() ? font : ImGui::GetFont();
}

// Flows one logical line as styled word units, wrapping at the window edge
// and drawing through the draw list. SameLine chaining cannot survive a
// wrapped segment (the next segment would start at the wrap edge), which is
// why the layout is computed manually here.
void RenderInlineRuns(
	const char* begin, const char* end, ImFont* bold, ImFont* code) noexcept {
	std::vector<coolhelper_shared::MarkdownInlineRun> runs;
	try {
		coolhelper_shared::CollectMarkdownInlineRuns(begin, end, runs);
	}
	catch (...) {
		ImGui::TextUnformatted(begin, end);
		return;
	}
	if (runs.empty())
		return;

	ImFont* fonts[5] = {};
	ImU32 colors[5] = {};
	fonts[0] = ImGui::GetFont();
	fonts[1] = bold ? bold : ImGui::GetFont();
	fonts[2] = code ? code : ImGui::GetFont();
	fonts[3] = ImGui::GetFont();
	fonts[4] = ImGui::GetFont();
	colors[0] = ImGui::GetColorU32(ImGuiCol_Text);
	colors[1] = colors[0];
	colors[2] = kInlineCodeColor;
	colors[3] = IM_COL32(125, 190, 255, 255);
	colors[4] = IM_COL32(150, 158, 170, 255);

	const float lineHeight = ImGui::GetTextLineHeight();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursor = ImGui::GetCursorScreenPos();
	const float startX = cursor.x;
	const float right = startX + ImGui::GetContentRegionAvail().x;
	float maxY = cursor.y + lineHeight;
	for (const coolhelper_shared::MarkdownInlineRun& run : runs) {
		ImFont* font = fonts[run.style];
		const bool codeBg = run.style == 2;
		const char* w = run.begin;
		if (codeBg) {
			while (w < run.end) {
				const char* firstEnd =
					coolhelper_shared::MarkdownWrapUnitEnd(w, run.end);
				const float firstWidth = font->CalcTextSizeA(
					font->FontSize, FLT_MAX, 0.0f, w, firstEnd).x;
				if (cursor.x + firstWidth > right && cursor.x > startX) {
					cursor.x = startX;
					cursor.y += lineHeight;
				}

				const char* segmentBegin = w;
				float segmentWidth = 0.0f;
				while (w < run.end) {
					const char* unitEnd =
						coolhelper_shared::MarkdownWrapUnitEnd(w, run.end);
					const float unitWidth = font->CalcTextSizeA(
						font->FontSize, FLT_MAX, 0.0f, w, unitEnd).x;
					if (cursor.x + segmentWidth + unitWidth > right &&
						segmentWidth > 0.0f)
						break;
					segmentWidth += unitWidth;
					w = unitEnd;
				}

				drawList->AddRectFilled(
					ImVec2(cursor.x - 2.0f, cursor.y),
					ImVec2(cursor.x + segmentWidth + 2.0f,
						cursor.y + lineHeight),
					kInlineCodeBackground, 2.0f);
				drawList->AddText(font, font->FontSize, cursor,
					colors[run.style], segmentBegin, w);
				cursor.x += segmentWidth;
				if (cursor.y + lineHeight > maxY)
					maxY = cursor.y + lineHeight;
				if (w < run.end) {
					cursor.x = startX;
					cursor.y += lineHeight;
				}
			}
			continue;
		}
		while (w < run.end) {
			const char* unitEnd =
				coolhelper_shared::MarkdownWrapUnitEnd(w, run.end);
			const float unitWidth = font->CalcTextSizeA(
				font->FontSize, FLT_MAX, 0.0f, w, unitEnd).x;
			if (cursor.x + unitWidth > right && cursor.x > startX) {
				cursor.x = startX;
				cursor.y += lineHeight;
			}
			drawList->AddText(font, font->FontSize, cursor,
				colors[run.style], w, unitEnd);
			cursor.x += unitWidth;
			if (cursor.y + lineHeight > maxY)
				maxY = cursor.y + lineHeight;
			w = unitEnd;
		}
	}
	ImGui::Dummy(ImVec2(0.0f, maxY - ImGui::GetCursorScreenPos().y));
}

} // namespace

void OverlayUi::SetFonts(ImFont* regular, ImFont* bold, ImFont* code) noexcept {
	font_ = regular;
	fontBold_ = bold;
	fontCode_ = code;
}

void OverlayUi::ApplyStyle() noexcept {
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 10.0f;
	style.WindowBorderSize = 1.0f;
	style.WindowPadding = ImVec2(12.0f, 10.0f);
	style.FrameRounding = 5.0f;
	style.FramePadding = ImVec2(9.0f, 4.0f);
	style.ItemSpacing = ImVec2(8.0f, 6.0f);
	style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
	style.CellPadding = ImVec2(6.0f, 3.0f);
	style.IndentSpacing = 16.0f;
	style.ScrollbarSize = 12.0f;
	style.ScrollbarRounding = 6.0f;
	style.GrabRounding = 4.0f;
	style.ChildRounding = 6.0f;
	style.PopupRounding = 6.0f;
	style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

	ImVec4* colors = style.Colors;
	// Translucent window: the overlay composites over the live desktop, so a
	// sub-1 alpha reveals the real content behind the glass.
	colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.070f, 0.098f, 0.86f);
	colors[ImGuiCol_TitleBg] = ImVec4(0.042f, 0.055f, 0.078f, 0.92f);
	colors[ImGuiCol_TitleBgActive] = ImVec4(0.070f, 0.090f, 0.125f, 0.95f);
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.042f, 0.055f, 0.078f, 0.60f);
	colors[ImGuiCol_Border] = ImVec4(0.45f, 0.58f, 0.78f, 0.30f);
	colors[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.97f, 1.0f);
	colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.61f, 0.69f, 1.0f);
	colors[ImGuiCol_Separator] = ImVec4(0.45f, 0.58f, 0.78f, 0.22f);
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.20f);
	colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.38f, 0.46f, 0.58f, 0.55f);
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.48f, 0.58f, 0.72f, 0.75f);
	colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.58f, 0.70f, 0.86f, 0.90f);
	colors[ImGuiCol_Button] = ImVec4(0.13f, 0.17f, 0.23f, 0.85f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.27f, 0.37f, 1.0f);
	colors[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.35f, 0.47f, 1.0f);
	colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	colors[ImGuiCol_Header] = ImVec4(0.16f, 0.22f, 0.30f, 0.60f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.30f, 0.40f, 0.80f);
	colors[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.36f, 0.48f, 1.0f);
}

UiFrameResult OverlayUi::Build(
	const UiSnapshot& snapshot, IAnswerProvider* answers,
	int answerScrollSteps) noexcept {
	UiFrameResult result = {};
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 work = viewport->WorkSize;

	const float remainingWidth = work.x - kOverlayMargin * 2.0f;
	const float remainingHeight = work.y - kOverlayMargin * 2.0f;
	const float availableWidth = remainingWidth > 1.0f ? remainingWidth : 1.0f;
	const float availableHeight = remainingHeight > 1.0f ? remainingHeight : 1.0f;
	const float windowWidth = availableWidth < kDefaultOverlayWidth
		? availableWidth : kDefaultOverlayWidth;
	const float windowHeight = availableHeight < kDefaultOverlayHeight
		? availableHeight : kDefaultOverlayHeight;
	ImGui::SetNextWindowPos(ImVec2(kOverlayMargin, kOverlayMargin),
		ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight),
		ImGuiCond_FirstUseEver);
	ImGui::Begin("DwmCfgOverlayLab", nullptr, ImGuiWindowFlags_NoCollapse);

	const float framerate = ImGui::GetIO().Framerate;
	ImGui::TextDisabled("(%.3f ms/frame | %.1f FPS)",
		framerate > 0.0f ? 1000.0f / framerate : 0.0f,
		framerate);
	if (snapshot.status[0])
		ImGui::TextDisabled("%s", snapshot.status);

	if (answers)
		BuildAnswerSection(answers, answerScrollSteps);

	result.windowPosition = ImGui::GetWindowPos();
	result.windowSize = ImGui::GetWindowSize();
	ImGui::End();
	return result;
}

void OverlayUi::BuildAnswerSection(
	IAnswerProvider* answers, int scrollSteps) noexcept {
	char errorBuffer[512] = {};
	char progressBuffer[512] = {};
	try {
		answerBuffer_.resize(static_cast<size_t>(kAnswerCopyCapacity));
	}
	catch (...) {
		answerBuffer_.clear();
	}
	const AnswerSnapshot snapshot = answers->CopySnapshot(
		answerBuffer_.empty() ? nullptr : answerBuffer_.data(),
		answerBuffer_.size(), errorBuffer, sizeof(errorBuffer),
		progressBuffer, sizeof(progressBuffer));
	const AnswerState state = snapshot.state;
	switch (state) {
	case AnswerState::Connecting:
		ImGui::TextColored(ImVec4(0.45f, 0.68f, 0.95f, 1.0f),
			"状态：正在连接服务");
		break;
	case AnswerState::Waiting:
		ImGui::TextColored(ImVec4(0.45f, 0.68f, 0.95f, 1.0f),
			"状态：等待模型响应");
		break;
	case AnswerState::Thinking:
		ImGui::TextColored(ImVec4(0.45f, 0.68f, 0.95f, 1.0f),
			"状态：模型正在推理");
		break;
	case AnswerState::Streaming:
		ImGui::TextColored(ImVec4(0.45f, 0.78f, 0.55f, 1.0f),
			"状态：正在生成答案");
		break;
	case AnswerState::Completed:
		ImGui::TextColored(ImVec4(0.45f, 0.78f, 0.55f, 1.0f),
			"\u72b6\u6001\uff1a\u5df2\u5b8c\u6210");
		break;
	case AnswerState::Failed:
		ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.38f, 1.0f),
			"\u72b6\u6001\uff1a\u5931\u8d25");
		break;
	case AnswerState::Cancelled:
		ImGui::TextUnformatted("\u72b6\u6001\uff1a\u5df2\u505c\u6b62");
		break;
	default:
		ImGui::TextUnformatted("\u72b6\u6001\uff1a\u7a7a\u95f2");
		break;
	}
	ImGui::SameLine();
	ImGui::TextDisabled(snapshot.hubConnected
		? "CoolHelperHub \u5df2\u8fde\u63a5"
		: "CoolHelperHub \u672a\u8fde\u63a5");
	if (snapshot.streamGap)
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
			"\u6ce8\u610f\uff1a\u901a\u4fe1\u79ef\u538b\u5bfc\u81f4\u4e2a\u522b\u7247\u6bb5\u4e22\u5931");
	if (snapshot.truncated)
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
			"\u6ce8\u610f\uff1a\u5185\u5bb9\u8d85\u8fc7\u4e0a\u9650\uff0c\u5df2\u622a\u65ad");
	ImGui::Separator();

	const bool streaming = state == AnswerState::Connecting ||
		state == AnswerState::Waiting || state == AnswerState::Thinking ||
		state == AnswerState::Streaming;
	ImGui::BeginChild("answerScroll", ImVec2(0.0f, 0.0f), 0, 0);
	// Scroll state is only meaningful inside the child; read it before the
	// content so SetScrollHereY can pin the view to the newest text.
	const bool atBottom = ImGui::GetScrollY() + ImGui::GetWindowHeight() >=
		ImGui::GetScrollMaxY() - 4.0f;

	if (errorBuffer[0]) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.38f, 1.0f));
		ImGui::TextWrapped("%s", errorBuffer);
		ImGui::PopStyleColor();
		ImGui::Separator();
	}
	if (state == AnswerState::Thinking && progressBuffer[0]) {
		ImGui::TextDisabled("模型推理：%s", progressBuffer);
		ImGui::Separator();
	}
	else if (state == AnswerState::Connecting) {
		ImGui::TextDisabled("正在连接并上传截图…");
		ImGui::Separator();
	}
	else if (state == AnswerState::Waiting) {
		ImGui::TextDisabled("服务已连接，等待模型响应…");
		ImGui::Separator();
	}

	const UINT64 length = snapshot.answerLength;
	if (length > 0) {
		RenderMarkdown(answerBuffer_.data());
	}
	else if (state == AnswerState::Idle && !errorBuffer[0]) {
		ImGui::TextDisabled(
			"\u622a\u56fe\u63d0\u95ee\u540e\uff0cAI \u56de\u7b54\u5c06\u4f1a\u5728\u8fd9\u91cc\u5b9e\u65f6\u663e\u793a");
	}

	if (scrollSteps != 0) {
		// Three text rows matches the common Windows mouse-wheel setting and
		// remains predictable across DPI and font-size changes.
		const float step = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
		ImGui::SetScrollY(ImGui::GetScrollY() + step * scrollSteps);
	}
	else if (streaming && atBottom)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

void OverlayUi::RenderMarkdown(const char* text) noexcept {
	codeBlockIndex_ = 0;
	try {
		coolhelper_shared::NormalizeMarkdownMath(text, normalizedText_);
		RenderBlockLines(normalizedText_.data(),
			normalizedText_.data() + normalizedText_.size());
	}
	catch (...) {
		ImGui::TextUnformatted(text);
	}
}

void OverlayUi::RenderBlockLines(
	const char* begin, const char* end) noexcept {
	bool inCodeFence = false;
	codeBuffer_.clear();

	// Wrap every text item at the window edge; code blocks use their own
	// child window (horizontal scroll, no wrap) and table cells push their
	// own wrap width, so neither is affected by this.
	ImGui::PushTextWrapPos(0.0f);
	const char* lineBegin = begin;
	while (lineBegin < end) {
		const char* lineEnd =
			coolhelper_shared::MarkdownFindChar(lineBegin, end, '\n');
		if (!lineEnd)
			lineEnd = end;
		const char* contentEnd = lineEnd;
		while (contentEnd > lineBegin &&
			(contentEnd[-1] == '\r' || contentEnd[-1] == ' '))
			--contentEnd;

		const bool isFence = contentEnd - lineBegin >= 3 &&
			lineBegin[0] == '`' && lineBegin[1] == '`' && lineBegin[2] == '`';
		if (inCodeFence) {
			if (isFence) {
				if (!codeBuffer_.empty())
					RenderCodeBlock(codeBuffer_.data(),
						codeBuffer_.data() + codeBuffer_.size());
				codeBuffer_.clear();
				inCodeFence = false;
			}
			else {
				codeBuffer_.append(lineBegin,
					static_cast<size_t>(contentEnd - lineBegin));
				codeBuffer_.push_back('\n');
			}
		}
		else if (isFence) {
			inCodeFence = true;
			codeBuffer_.clear();
		}
		else if (coolhelper_shared::MarkdownIsBlankLine(
			lineBegin, contentEnd)) {
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.40f));
		}
		else {
			int leading = 0;
			const char* p = lineBegin;
			while (p < contentEnd && (*p == ' ' || *p == '\t')) {
				leading += *p == '\t' ? 4 : 1;
				++p;
			}
			const float indentPx = static_cast<float>(
				(leading / 2 > 3 ? 3 : leading / 2) * 14);

			// Markdown pipe tables: collect the run of '|'-lines and render
			// them as aligned columns when the separator row validates.
			if (p < contentEnd && p[0] == '|') {
				tableRows_.clear();
				const char* scan = lineBegin;
				while (scan < end && tableRows_.size() < 64) {
					const char* scanEnd =
						coolhelper_shared::MarkdownFindChar(scan, end, '\n');
					if (!scanEnd)
						scanEnd = end;
					const char* cellEnd = scanEnd;
					while (cellEnd > scan &&
						(cellEnd[-1] == '\r' || cellEnd[-1] == ' '))
						--cellEnd;
					const char* cellStart =
						coolhelper_shared::MarkdownSkipSpaces(scan, cellEnd);
					if (cellStart >= cellEnd || cellStart[0] != '|')
						break;
					tableRows_.emplace_back();
					if (!coolhelper_shared::MarkdownParseTableRow(
						cellStart, cellEnd, &tableRows_.back())) {
						tableRows_.pop_back();
						break;
					}
					scan = scanEnd < end ? scanEnd + 1 : end;
				}
				if (tableRows_.size() >= 2 &&
					tableRows_[0].size() == tableRows_[1].size() &&
					tableRows_[0].size() <= 8) {
					bool separatorOk = true;
					for (const std::string& cell : tableRows_[1]) {
						if (!coolhelper_shared::MarkdownIsTableSeparatorCell(cell)) {
							separatorOk = false;
							break;
						}
					}
					if (separatorOk) {
						if (indentPx > 0.0f)
							ImGui::Indent(indentPx);
						RenderMarkdownTable();
						if (indentPx > 0.0f)
							ImGui::Unindent(indentPx);
						lineBegin = scan;
						continue;
					}
				}
			}

			if (p < contentEnd) {
				if (indentPx > 0.0f)
					ImGui::Indent(indentPx);

				const bool isHr = contentEnd - p >= 3 &&
					(p[0] == '-' || p[0] == '*' || p[0] == '_') &&
					coolhelper_shared::MarkdownIsHorizontalRule(p, contentEnd);
				const bool isBullet = contentEnd - p >= 2 &&
					(p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ';
				const char* orderedContent = nullptr;
				const bool isOrdered =
					!isHr && coolhelper_shared::MarkdownParseOrderedListMarker(
						p, contentEnd, &orderedContent);
				const bool isQuote = p[0] == '>';
				int headingLevel = 0;
				if (p < contentEnd && p[0] == '#') {
					const char* hashes = p;
					while (hashes < contentEnd && *hashes == '#' && headingLevel < 6) {
						++headingLevel;
						++hashes;
					}
					if (hashes >= contentEnd || *hashes != ' ')
						headingLevel = 0;
				}

				if (isHr) {
					ImGui::Separator();
				}
				else if (isBullet) {
					ImGui::PushStyleColor(ImGuiCol_Text,
						ImGui::ColorConvertU32ToFloat4(kAccentColor));
					ImGui::Bullet();
					ImGui::PopStyleColor();
					RenderInlineRuns(p + 2, contentEnd, fontBold_, fontCode_);
				}
				else if (isOrdered) {
					const char* numberEnd = orderedContent - 2;
					ImGui::PushStyleColor(ImGuiCol_Text,
						ImGui::ColorConvertU32ToFloat4(kAccentColor));
					ImGui::TextUnformatted(p, numberEnd);
					ImGui::PopStyleColor();
					ImGui::SameLine(0.0f, 3.0f);
					RenderInlineRuns(orderedContent, contentEnd, fontBold_, fontCode_);
				}
				else if (isQuote) {
					const char* quoteText =
						coolhelper_shared::MarkdownSkipSpaces(p + 1, contentEnd);
					ImGui::Indent(10.0f);
					ImGui::PushStyleColor(ImGuiCol_Text,
						ImGui::ColorConvertU32ToFloat4(kQuoteColor));
					ImGui::TextUnformatted(quoteText, contentEnd);
					ImGui::PopStyleColor();
					const ImVec2 quoteMin = ImGui::GetItemRectMin();
					const ImVec2 quoteMax = ImGui::GetItemRectMax();
					ImGui::GetWindowDrawList()->AddRectFilled(
						ImVec2(quoteMin.x - 8.0f, quoteMin.y - 1.0f),
						ImVec2(quoteMin.x - 5.0f, quoteMax.y + 1.0f),
						kQuoteBarColor, 1.5f);
					ImGui::Unindent(10.0f);
				}
				else if (headingLevel > 0) {
					const char* headingText =
						coolhelper_shared::MarkdownSkipSpaces(
							p + headingLevel, contentEnd);
					ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.20f));
					ImGui::PushStyleColor(ImGuiCol_Text,
						ImGui::ColorConvertU32ToFloat4(kHeadingColor));
					if (fontBold_) {
						ImGui::PushFont(FontOrDefault(fontBold_));
						ImGui::SetWindowFontScale(
							1.0f + (3.0f - headingLevel) * 0.14f);
						ImGui::TextUnformatted(headingText, contentEnd);
						ImGui::SetWindowFontScale(1.0f);
						ImGui::PopFont();
					}
					else {
						ImGui::TextUnformatted(headingText, contentEnd);
					}
					ImGui::PopStyleColor();
					if (headingLevel <= 2) {
						const ImVec2 headingMin = ImGui::GetItemRectMin();
						const float lineY = ImGui::GetItemRectMax().y + 3.0f;
						const float right = headingMin.x +
							ImGui::GetContentRegionAvail().x;
						ImGui::GetWindowDrawList()->AddLine(
							ImVec2(headingMin.x, lineY), ImVec2(right, lineY),
							kHeadingUnderline,
							headingLevel == 1 ? 2.0f : 1.0f);
					}
					ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.25f));
				}
				else {
					RenderInlineRuns(p, contentEnd, fontBold_, fontCode_);
				}

				if (indentPx > 0.0f)
					ImGui::Unindent(indentPx);
			}
		}

		lineBegin = lineEnd < end ? lineEnd + 1 : end;
	}
	ImGui::PopTextWrapPos();

	if (inCodeFence && !codeBuffer_.empty())
		RenderCodeBlock(codeBuffer_.data(), codeBuffer_.data() + codeBuffer_.size());
}

void OverlayUi::RenderCodeBlock(const char* begin, const char* end) noexcept {
	ImGui::PushStyleColor(ImGuiCol_ChildBg,
		ImGui::ColorConvertU32ToFloat4(kCodeBackground));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
	const float lineCount =
		static_cast<float>(std::count(begin, end, '\n')) + 1.0f;
	const float height = lineCount * ImGui::GetTextLineHeight() +
		ImGui::GetStyle().WindowPadding.y * 2.0f;
	ImGui::PushID(codeBlockIndex_++);
	ImGui::PushStyleColor(ImGuiCol_Border,
		ImGui::ColorConvertU32ToFloat4(kTableLine));
	if (ImGui::BeginChild("##codeBlock", ImVec2(0.0f, height),
		ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
		if (fontCode_) {
			ImGui::PushFont(FontOrDefault(fontCode_));
			ImGui::TextUnformatted(begin, end);
			ImGui::PopFont();
		}
		else {
			ImGui::TextUnformatted(begin, end);
		}
	}
	// Border and rounding are read when the child renders in EndChild, so the
	// style pops must happen after it.
	ImGui::EndChild();
	ImGui::PopID();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.35f));
}

void OverlayUi::RenderMarkdownTable() noexcept {
	if (tableRows_.empty())
		return;
	const size_t columnCount = tableRows_[0].size();
	if (columnCount == 0)
		return;
	ImFont* bold = fontBold_ ? FontOrDefault(fontBold_) : nullptr;
	constexpr float padX = 8.0f;
	constexpr float padY = 3.0f;

	// Measure column widths with the font each row renders in.
	tableWidths_.assign(columnCount, 0.0f);
	for (size_t row = 0; row < tableRows_.size(); ++row) {
		const std::vector<std::string>& cells = tableRows_[row];
		const size_t count = cells.size() < columnCount ? cells.size() : columnCount;
		for (size_t column = 0; column < count; ++column) {
			const std::string& cell = cells[column];
			float width = 0.0f;
			if (row == 0 && bold) {
				ImGui::PushFont(bold);
				width = ImGui::CalcTextSize(
					cell.data(), cell.data() + cell.size()).x;
				ImGui::PopFont();
			}
			else {
				width = ImGui::CalcTextSize(
					cell.data(), cell.data() + cell.size()).x;
			}
			tableWidths_[column] = (tableWidths_[column] > width)
				? tableWidths_[column] : width;
		}
	}
	float total = 0.0f;
	for (float& width : tableWidths_) {
		width += padX * 2.0f;
		total += width;
	}
	const float available = ImGui::GetContentRegionAvail().x;
	if (total > available && total > 0.0f) {
		const float scale = available / total;
		total = 0.0f;
		for (float& width : tableWidths_) {
			width *= scale;
			total += width;
		}
	}

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImVec2 start = ImGui::GetCursorScreenPos();
	float y = start.y;
	for (size_t row = 0; row < tableRows_.size(); ++row) {
		const std::vector<std::string>& cells = tableRows_[row];
		float x = start.x;
		float rowHeight = ImGui::GetTextLineHeight() + padY * 2.0f;
		const bool isHeader = row == 0;
		for (size_t column = 0; column < columnCount; ++column) {
			ImGui::SetCursorScreenPos(
				ImVec2(x + padX, y + padY));
			ImGui::PushTextWrapPos(x + tableWidths_[column] - padX);
			const bool pushedFont = isHeader && bold != nullptr;
			if (pushedFont)
				ImGui::PushFont(bold);
			if (column < cells.size() && !cells[column].empty())
				ImGui::TextUnformatted(cells[column].data(),
					cells[column].data() + cells[column].size());
			else
				ImGui::Dummy(ImVec2(0.0f, 0.0f));
			if (pushedFont)
				ImGui::PopFont();
			ImGui::PopTextWrapPos();
			const float cellBottom = ImGui::GetItemRectMax().y + padY;
			if (cellBottom - y > rowHeight)
				rowHeight = cellBottom - y;
			x += tableWidths_[column];
		}
		drawList->AddLine(ImVec2(start.x, y + rowHeight),
			ImVec2(start.x + total, y + rowHeight),
			isHeader ? kTableHeaderLine : kTableLine,
			isHeader ? 2.0f : 1.0f);
		y += rowHeight;
	}
	ImGui::SetCursorScreenPos(ImVec2(start.x, y));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.35f));
}

} // namespace dwm_overlay
