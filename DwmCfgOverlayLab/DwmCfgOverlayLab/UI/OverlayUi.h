#pragma once

#include "../IPC/UiState.h"
#include "../IMGUI/imgui.h"

#include <string>
#include <vector>

namespace dwm_overlay {

struct UiFrameResult {
	ImVec2 windowPosition = {};
	ImVec2 windowSize = {};
};

class OverlayUi final {
public:
	// Fonts are owned by the renderer's ImFontAtlas; they may be null, in which
	// case rendering falls back to the current font without emphasis.
	void SetFonts(ImFont* regular, ImFont* bold, ImFont* code) noexcept;
	// Dark glass look: translucent window over the live desktop composition.
	void ApplyStyle() noexcept;
	UiFrameResult Build(const UiSnapshot& snapshot, IAnswerProvider* answers,
		int answerScrollSteps) noexcept;

private:
	void BuildAnswerSection(IAnswerProvider* answers, int scrollSteps) noexcept;
	void RenderMarkdown(const char* text) noexcept;
	void RenderBlockLines(const char* begin, const char* end) noexcept;
	void RenderCodeBlock(const char* begin, const char* end) noexcept;
	void RenderMarkdownTable() noexcept;

	ImFont* font_ = nullptr;
	ImFont* fontBold_ = nullptr;
	ImFont* fontCode_ = nullptr;
	int codeBlockIndex_ = 0;
	// Reused scratch buffers so a busy frame never reallocates needlessly.
	std::string codeBuffer_;
	std::vector<char> answerBuffer_;
	std::vector<std::vector<std::string>> tableRows_;
	std::vector<float> tableWidths_;
};

} // namespace dwm_overlay
