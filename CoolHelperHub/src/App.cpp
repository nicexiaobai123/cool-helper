#include "coolhelper/App.h"

#include "coolhelper/DllInjector.h"
#include "coolhelper/Logger.h"
#include "coolhelper/Screenshot.h"
#include "resource.h"
#include "../../Shared/MarkdownText.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string_view>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace coolhelper {
namespace {

constexpr UINT kTrayShow = 1001;
constexpr UINT kTrayExit = 1003;

// Exact static UI text also contributes punctuation and symbols outside the
// core CJK range. Streamed answer text is backfilled dynamically as needed.
constexpr char kStaticUiGlyphSeed[] =
	"面试截图助手答案设置覆盖层接口配置提示词预设综合算法题系统问题"
	"全局快捷键截图并提问切换显示隐藏当前保存后生效配置文件路径恢复默认"
	"注入卸载状态进程已未找到连接中未知控制刷新安全管理员权限成功失败错误"
	"请输入模型地址密钥使用仅能当前用户解密完成停止清空复制内容等待接收"
	"字体字形窗口工具栏主题选项开关常见设置支持快捷组合按键覆盖显示层"
	"答案向上下滚动";

constexpr ImWchar kSymbolGlyphRanges[] = {
	0x00B1, 0x00B1, // plus-minus
	0x00D7, 0x00D7, // multiplication sign
	0x00F7, 0x00F7, // division sign
	0x0391, 0x03C9, // Greek letters used by common complexity notation
	0x2000, 0x2BFF, // punctuation, arrows, math operators and common symbols
	0
};

void MergeSymbolFont(ImFontAtlas* fonts, float size) noexcept {
	ImFontConfig config = {};
	config.MergeMode = true;
	config.OversampleH = 2;
	config.OversampleV = 2;
	fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf",
		size, &config, kSymbolGlyphRanges);
}

bool BuildAndUploadHubFonts(const char* reason) noexcept {
	ImGuiIO& io = ImGui::GetIO();
	if (!io.Fonts->Build()) {
		Logger::Write(LogLevel::Error, "Font atlas build failed");
		return false;
	}

	const int width = io.Fonts->TexWidth;
	const int height = io.Fonts->TexHeight;
	const std::string dimensions = "Font atlas (" + std::string(reason) +
		"): " + std::to_string(width) + "x" + std::to_string(height);
	Logger::Write(LogLevel::Info, dimensions.c_str());
	if (width <= 0 || height <= 0 ||
		width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
		Logger::Write(LogLevel::Error,
			"Font atlas exceeds the D3D11 texture size limit");
		return false;
	}

	ImGui_ImplDX11_InvalidateDeviceObjects();
	if (!ImGui_ImplDX11_CreateDeviceObjects()) {
		Logger::Write(LogLevel::Error,
			"D3D11 font texture creation failed");
		return false;
	}
	return true;
}

struct HotkeyOption {
	const char* name;
	UINT virtualKey;
};

constexpr HotkeyOption kHotkeyOptions[] = {
	{ "A", 'A' }, { "B", 'B' }, { "C", 'C' }, { "D", 'D' },
	{ "E", 'E' }, { "F", 'F' }, { "G", 'G' }, { "H", 'H' },
	{ "I", 'I' }, { "J", 'J' }, { "K", 'K' }, { "L", 'L' },
	{ "M", 'M' }, { "N", 'N' }, { "O", 'O' }, { "P", 'P' },
	{ "Q", 'Q' }, { "R", 'R' }, { "S", 'S' }, { "T", 'T' },
	{ "U", 'U' }, { "V", 'V' }, { "W", 'W' }, { "X", 'X' },
	{ "Y", 'Y' }, { "Z", 'Z' },
	{ "0", '0' }, { "1", '1' }, { "2", '2' }, { "3", '3' },
	{ "4", '4' }, { "5", '5' }, { "6", '6' }, { "7", '7' },
	{ "8", '8' }, { "9", '9' },
	{ "+", VK_OEM_PLUS }, { "-", VK_OEM_MINUS },
	{ "Num +", VK_ADD }, { "Num -", VK_SUBTRACT },
	{ "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 },
	{ "F4", VK_F4 }, { "F5", VK_F5 }, { "F6", VK_F6 },
	{ "F7", VK_F7 }, { "F8", VK_F8 }, { "F9", VK_F9 },
	{ "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 }
};

const char* HotkeyKeyName(std::uint32_t virtualKey) noexcept {
	for (const auto& option : kHotkeyOptions) {
		if (option.virtualKey == virtualKey)
			return option.name;
	}
	return "?";
}

std::string HotkeyText(const CaptureHotkeySettings& hotkey) {
	std::string result;
	const auto append = [&result](std::string_view part) {
		if (!result.empty())
			result += '+';
		result += part;
	};
	if (hotkey.control) append("Ctrl");
	if (hotkey.alt) append("Alt");
	if (hotkey.shift) append("Shift");
	if (hotkey.windows) append("Win");
	append(HotkeyKeyName(hotkey.virtualKey));
	return result;
}

std::wstring AsciiToWide(std::string_view text) {
	return std::wstring(text.begin(), text.end());
}

UINT NativeHotkeyModifiers(const CaptureHotkeySettings& hotkey) noexcept {
	UINT modifiers = MOD_NOREPEAT;
	if (hotkey.control) modifiers |= MOD_CONTROL;
	if (hotkey.alt) modifiers |= MOD_ALT;
	if (hotkey.shift) modifiers |= MOD_SHIFT;
	if (hotkey.windows) modifiers |= MOD_WIN;
	return modifiers;
}

bool IsOrdinaryUnmodifiedKey(const CaptureHotkeySettings& hotkey) noexcept {
	const bool hasModifier = hotkey.control || hotkey.alt ||
		hotkey.shift || hotkey.windows;
	const bool isLetterOrDigit =
		(hotkey.virtualKey >= 'A' && hotkey.virtualKey <= 'Z') ||
		(hotkey.virtualKey >= '0' && hotkey.virtualKey <= '9');
	return !hasModifier && isLetterOrDigit;
}

std::string WideToUtf8(std::wstring_view text) {
	if (text.empty())
		return {};
	const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(),
		static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0)
		return {};
	std::string result(static_cast<std::size_t>(required), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		result.data(), required, nullptr, nullptr);
	return result;
}

std::wstring Utf8ToWide(std::string_view text) {
	if (text.empty())
		return {};
	const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (required <= 0)
		return {};
	std::wstring result(static_cast<std::size_t>(required), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
		static_cast<int>(text.size()), result.data(), required);
	return result;
}

template <std::size_t Size>
void CopyField(std::array<char, Size>& target, std::string_view source) noexcept {
	const std::size_t count = (std::min)(source.size(), Size - 1);
	std::memcpy(target.data(), source.data(), count);
	target[count] = '\0';
}

bool IsBlank(std::string_view value) noexcept {
	return std::all_of(value.begin(), value.end(), [](unsigned char character) {
		return std::isspace(character) != 0;
	});
}

// ---------------------------------------------------------------------------
// Shared UI theme: dark surface with a blue accent, matching the DWM overlay
// glass style so both programs read as one product.
constexpr ImVec4 kAccent(0.376f, 0.647f, 0.984f, 1.0f);
constexpr ImVec4 kAccentDeep(0.204f, 0.408f, 0.678f, 1.0f);
constexpr ImVec4 kSuccess(0.420f, 0.780f, 0.553f, 1.0f);
constexpr ImVec4 kError(0.953f, 0.424f, 0.384f, 1.0f);
constexpr ImVec4 kWarning(0.976f, 0.733f, 0.310f, 1.0f);
constexpr ImVec4 kFaint(0.520f, 0.573f, 0.651f, 1.0f);

ImVec4 WithAlpha(const ImVec4& color, float alpha) noexcept {
	return ImVec4(color.x, color.y, color.z, alpha);
}

ImVec4 StateColor(RequestState state) noexcept {
	switch (state) {
	case RequestState::Capturing: return kWarning;
	case RequestState::Connecting:
	case RequestState::Waiting:
	case RequestState::Thinking:
	case RequestState::Streaming: return kAccent;
	case RequestState::Completed: return kSuccess;
	case RequestState::Error: return kError;
	default: return kFaint;
	}
}

bool IsActiveRequestState(RequestState state) noexcept {
	return state == RequestState::Connecting ||
		state == RequestState::Waiting ||
		state == RequestState::Thinking ||
		state == RequestState::Streaming;
}

bool LooksLikeError(std::string_view message) noexcept {
	return message.find("失败") != std::string_view::npos ||
		message.find("错误") != std::string_view::npos ||
		message.find("无法") != std::string_view::npos ||
		message.find("占用") != std::string_view::npos;
}

// Pill-shaped status badge: colored dot + label inside a rounded frame. Built
// from the draw list plus a Dummy so it participates in normal layout.
constexpr float kChipPadX = 9.0f;
constexpr float kChipPadY = 4.0f;
constexpr float kChipDotSpace = 12.0f;

float ChipWidth(std::string_view label) noexcept {
	return kChipPadX + kChipDotSpace +
		ImGui::CalcTextSize(label.data(), label.data() + label.size()).x +
		kChipPadX;
}

void StatusChip(std::string_view label, const ImVec4& color) noexcept {
	const ImVec2 textSize = ImGui::CalcTextSize(
		label.data(), label.data() + label.size());
	const ImVec2 size(ChipWidth(label), textSize.y + kChipPadY * 2.0f);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
		ImGui::ColorConvertFloat4ToU32(WithAlpha(color, 0.14f)),
		size.y * 0.5f);
	drawList->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y),
		ImGui::ColorConvertFloat4ToU32(WithAlpha(color, 0.45f)),
		size.y * 0.5f, 0, 1.0f);
	drawList->AddCircleFilled(
		ImVec2(origin.x + kChipPadX + 4.0f, origin.y + size.y * 0.5f), 3.5f,
		ImGui::ColorConvertFloat4ToU32(color));
	drawList->AddText(
		ImVec2(origin.x + kChipPadX + kChipDotSpace, origin.y + kChipPadY),
		ImGui::GetColorU32(ImGuiCol_Text), label.data(),
		label.data() + label.size());
	ImGui::Dummy(size);
}

// Vertically centers plain text against a StatusChip placed on the same row.
void ChipAlignedText(std::string_view text) noexcept {
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + kChipPadY);
	ImGui::TextDisabled("%.*s", static_cast<int>(text.size()), text.data());
}

// Primary action: filled accent button with dark text.
constexpr ImVec2 kButtonPadding(14.0f, 7.0f);

bool AccentButton(const char* label) noexcept {
	ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		ImVec4(0.478f, 0.725f, 1.0f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDeep);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.055f, 0.086f, 0.137f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kButtonPadding);
	const bool clicked = ImGui::Button(label);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
	return clicked;
}

// Destructive action: filled red button with light text.
bool DangerButton(const char* label) noexcept {
	ImGui::PushStyleColor(ImGuiCol_Button, kError);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		ImVec4(1.0f, 0.51f, 0.46f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,
		ImVec4(0.80f, 0.32f, 0.29f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.97f, 0.96f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kButtonPadding);
	const bool clicked = ImGui::Button(label);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
	return clicked;
}

// Secondary action: default colors but sized to line up with primary and
// danger buttons in the same row.
bool SecondaryButton(const char* label) noexcept {
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kButtonPadding);
	const bool clicked = ImGui::Button(label);
	ImGui::PopStyleVar();
	return clicked;
}

// Framed notice box used for errors and confirmations.
void NoticeBox(const char* id, std::string_view text, const ImVec4& color) noexcept {
	ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(color, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(color, 0.45f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
	if (ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextWrapped("%.*s",
			static_cast<int>(text.size()), text.data());
		ImGui::PopStyleColor();
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(2);
}

void DisabledTextWrapped(std::string_view text) noexcept {
	ImGui::PushStyleColor(ImGuiCol_Text,
		ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%.*s", static_cast<int>(text.size()), text.data());
	ImGui::PopStyleColor();
}

void ColoredTextWrapped(std::string_view text, const ImVec4& color) noexcept {
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	ImGui::TextWrapped("%.*s", static_cast<int>(text.size()), text.data());
	ImGui::PopStyleColor();
}

float ActionButtonWidth(const char* label) noexcept {
	return ImGui::CalcTextSize(label).x + kButtonPadding.x * 2.0f;
}

void SameLineIfFits(float nextItemWidth) noexcept {
	const float right = ImGui::GetWindowPos().x +
		ImGui::GetWindowContentRegionMax().x;
	const float nextRight = ImGui::GetItemRectMax().x +
		ImGui::GetStyle().ItemSpacing.x + nextItemWidth;
	if (nextRight <= right)
		ImGui::SameLine();
}

void SameLineForButtonIfFits(const char* nextLabel) noexcept {
	SameLineIfFits(ActionButtonWidth(nextLabel));
}

float CheckboxWidth(const char* label) noexcept {
	return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
		ImGui::CalcTextSize(label).x;
}

// Disabled text pushed to the right edge of the current row.
void RightDisabledText(std::string_view text) noexcept {
	const float width = ImGui::CalcTextSize(
		text.data(), text.data() + text.size()).x;
	const float currentX = ImGui::GetCursorPosX();
	const float x = currentX + ImGui::GetContentRegionAvail().x - width;
	if (x <= currentX + ImGui::GetStyle().ItemSpacing.x) {
		// A narrow or high-DPI window cannot hold this status text beside the
		// title.  Give it its own line instead of drawing over the title.
		ImGui::NewLine();
		DisabledTextWrapped(text);
		return;
	}
	ImGui::SameLine(x);
	ImGui::TextDisabled("%.*s", static_cast<int>(text.size()), text.data());
}

void ApplyHubTheme() noexcept {
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowPadding = ImVec2(14.0f, 12.0f);
	style.FramePadding = ImVec2(9.0f, 6.0f);
	style.ItemSpacing = ImVec2(8.0f, 8.0f);
	style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
	style.IndentSpacing = 18.0f;
	style.WindowRounding = 8.0f;
	style.ChildRounding = 8.0f;
	style.FrameRounding = 6.0f;
	style.PopupRounding = 8.0f;
	style.TabRounding = 6.0f;
	style.ScrollbarSize = 13.0f;
	style.ScrollbarRounding = 7.0f;
	style.GrabRounding = 5.0f;
	style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
	style.SeparatorTextPadding = ImVec2(0.0f, 6.0f);

	ImVec4* colors = style.Colors;
	colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.065f, 0.085f, 1.0f);
	colors[ImGuiCol_ChildBg] = ImVec4(0.070f, 0.082f, 0.104f, 0.45f);
	colors[ImGuiCol_PopupBg] = ImVec4(0.070f, 0.085f, 0.110f, 0.97f);
	colors[ImGuiCol_Border] = ImVec4(0.25f, 0.30f, 0.38f, 0.35f);
	colors[ImGuiCol_Text] = ImVec4(0.922f, 0.940f, 0.970f, 1.0f);
	colors[ImGuiCol_TextDisabled] = kFaint;
	colors[ImGuiCol_FrameBg] = ImVec4(0.100f, 0.115f, 0.150f, 1.0f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(0.140f, 0.160f, 0.210f, 1.0f);
	colors[ImGuiCol_FrameBgActive] = ImVec4(0.170f, 0.190f, 0.250f, 1.0f);
	colors[ImGuiCol_Button] = ImVec4(0.140f, 0.160f, 0.210f, 1.0f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(0.190f, 0.220f, 0.290f, 1.0f);
	colors[ImGuiCol_ButtonActive] = ImVec4(0.230f, 0.260f, 0.340f, 1.0f);
	colors[ImGuiCol_CheckMark] = kAccent;
	colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.30f, 0.38f, 0.30f);
	colors[ImGuiCol_Tab] = ImVec4(0.055f, 0.065f, 0.085f, 1.0f);
	colors[ImGuiCol_TabHovered] = WithAlpha(kAccent, 0.22f);
	colors[ImGuiCol_TabActive] = ImVec4(0.115f, 0.140f, 0.185f, 1.0f);
	colors[ImGuiCol_TabUnfocused] = ImVec4(0.055f, 0.065f, 0.085f, 1.0f);
	colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.100f, 0.120f, 0.160f, 1.0f);
	colors[ImGuiCol_TitleBg] = ImVec4(0.045f, 0.053f, 0.070f, 1.0f);
	colors[ImGuiCol_TitleBgActive] = ImVec4(0.045f, 0.053f, 0.070f, 1.0f);
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.15f);
	colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.35f, 0.44f, 0.55f);
	colors[ImGuiCol_ScrollbarGrabHovered] =
		ImVec4(0.40f, 0.47f, 0.57f, 0.80f);
	colors[ImGuiCol_ScrollbarGrabActive] =
		ImVec4(0.48f, 0.56f, 0.68f, 0.95f);
	colors[ImGuiCol_Header] = WithAlpha(kAccent, 0.16f);
	colors[ImGuiCol_HeaderHovered] = WithAlpha(kAccent, 0.26f);
	colors[ImGuiCol_HeaderActive] = WithAlpha(kAccent, 0.36f);
}

// ---------------------------------------------------------------------------
// Markdown rendering for the answer page (subset ported from the DWM overlay
// renderer): headings, bold, inline code, fenced code blocks, bullets and
// ordered lists, blockquotes, pipe tables, links, horizontal rules.
constexpr ImU32 kMdCodeBackground = IM_COL32(24, 28, 34, 235);
constexpr ImU32 kMdHeadingColor = IM_COL32(236, 241, 250, 255);
constexpr ImU32 kMdHeadingUnderline = IM_COL32(96, 165, 250, 150);
constexpr ImU32 kMdQuoteColor = IM_COL32(158, 168, 182, 255);
constexpr ImU32 kMdQuoteBarColor = IM_COL32(96, 165, 250, 140);
constexpr ImU32 kMdInlineCodeColor = IM_COL32(165, 225, 195, 255);
constexpr ImU32 kMdInlineCodeBackground = IM_COL32(255, 255, 255, 22);
constexpr ImU32 kMdTableLine = IM_COL32(255, 255, 255, 34);
constexpr ImU32 kMdTableHeaderLine = IM_COL32(96, 165, 250, 170);

const char* MdFindChar(const char* begin, const char* end, char value) noexcept {
	const void* found = memchr(begin, value, static_cast<size_t>(end - begin));
	return static_cast<const char*>(found);
}

const char* MdFindDoubleChar(const char* begin, const char* end, char value) noexcept {
	for (const char* scan = begin; scan + 1 < end; ++scan) {
		if (scan[0] == value && scan[1] == value)
			return scan;
	}
	return nullptr;
}

const char* MdSkipSpaces(const char* p, const char* end) noexcept {
	while (p < end && (*p == ' ' || *p == '\t'))
		++p;
	return p;
}

bool MdIsBlankLine(const char* begin, const char* end) noexcept {
	for (const char* p = begin; p < end; ++p) {
		if (*p != ' ' && *p != '\t')
			return false;
	}
	return true;
}

bool MdIsHorizontalRule(const char* begin, const char* end) noexcept {
	for (const char* p = begin; p < end; ++p) {
		if (*p != begin[0] && *p != ' ')
			return false;
	}
	return true;
}

bool MdParseOrderedListMarker(
	const char* p, const char* end, const char** contentBegin) noexcept {
	if (p >= end || *p < '0' || *p > '9')
		return false;
	const char* q = p;
	int digits = 0;
	while (q < end && *q >= '0' && *q <= '9' && digits < 4) {
		++q;
		++digits;
	}
	if (q + 1 < end && (*q == '.' || *q == ')') && q[1] == ' ') {
		*contentBegin = q + 2;
		return true;
	}
	return false;
}

bool MdIsTableSeparatorCell(const std::string& cell) noexcept {
	size_t index = 0;
	const size_t size = cell.size();
	while (index < size && (cell[index] == ' ' || cell[index] == '\t'))
		++index;
	if (index < size && cell[index] == ':')
		++index;
	size_t dashes = 0;
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

bool MdParseTableRow(
	const char* begin, const char* end, std::vector<std::string>* cells) noexcept {
	cells->clear();
	const char* p = begin + 1; // caller guarantees begin[0] == '|'
	std::string cell;
	while (p <= end) {
		if (p == end || *p == '|') {
			size_t lead = 0;
			size_t trail = cell.size();
			while (lead < trail && (cell[lead] == ' ' || cell[lead] == '\t'))
				++lead;
			while (trail > lead && (cell[trail - 1] == ' ' || cell[trail - 1] == '\t'))
				--trail;
			cells->emplace_back(cell, lead, trail - lead);
			cell.clear();
			if (p == end)
				break;
			++p;
			continue;
		}
		cell.push_back(*p++);
	}
	if (!cells->empty() && cells->back().empty())
		cells->pop_back();
	return !cells->empty();
}

// Splits a logical line into styled runs without touching ImGui state.
struct MdRun {
	const char* begin;
	const char* end;
	int style; // 0 plain, 1 bold, 2 code, 3 link text, 4 link url
};

void MdCollectRuns(const char* begin, const char* end, std::vector<MdRun>& runs) noexcept {
	const char* p = begin;
	while (p < end) {
		const char* doubleStar = MdFindDoubleChar(p, end, '*');
		const char* backtick = MdFindChar(p, end, '`');
		const char* linkStart = nullptr;
		for (const char* scan = p; scan < end;) {
			const char* bracket = MdFindChar(scan, end, '[');
			if (!bracket)
				break;
			const char* closeBracket = MdFindChar(bracket + 1, end, ']');
			if (closeBracket && closeBracket + 1 < end &&
				closeBracket[1] == '(' &&
				MdFindChar(closeBracket + 2, end, ')')) {
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
			runs.push_back({p, end, 0});
			return;
		}
		if (marker > p)
			runs.push_back({p, marker, 0});
		if (marker == linkStart) {
			const char* closeBracket = MdFindChar(marker + 1, end, ']');
			const char* urlClose = MdFindChar(closeBracket + 2, end, ')');
			runs.push_back({marker + 1, closeBracket, 3});
			runs.push_back({closeBracket + 1, urlClose + 1, 4});
			p = urlClose + 1;
		}
		else if (*marker == '`') {
			const char* closing = MdFindChar(marker + 1, end, '`');
			if (!closing) {
				runs.push_back({marker, end, 0});
				return;
			}
			runs.push_back({marker + 1, closing, 2});
			p = closing + 1;
		}
		else {
			const char* closing = MdFindDoubleChar(marker + 2, end, '*');
			if (!closing) {
				runs.push_back({marker, end, 0});
				return;
			}
			runs.push_back({marker + 2, closing, 1});
			p = closing + 2;
		}
	}
}

unsigned MdDecodeUtf8(const char* s, const char* end, int* length) noexcept {
	unsigned c = static_cast<unsigned char>(*s);
	*length = 1;
	if (c < 0x80)
		return c;
	if ((c & 0xE0) == 0xC0 && s + 1 < end) {
		*length = 2;
		return ((c & 0x1F) << 6) | (s[1] & 0x3F);
	}
	if ((c & 0xF0) == 0xE0 && s + 2 < end) {
		*length = 3;
		return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
	}
	if ((c & 0xF8) == 0xF0 && s + 3 < end) {
		*length = 4;
		return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
			((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
	}
	return c;
}

// CJK and fullwidth characters allow a line break on either side.
bool MdIsWideCodepoint(unsigned codepoint) noexcept {
	return codepoint >= 0x1100 && (codepoint <= 0x11FF || codepoint >= 0x2E80);
}

// Flows one logical line as styled word units, wrapping at the window edge
// and drawing through the draw list. SameLine chaining cannot survive a
// wrapped segment (the next segment would start at the wrap edge), which is
// why the layout is computed manually here.
void MdRenderInlineRuns(
	const char* begin, const char* end, ImFont* bold, ImFont* code) noexcept {
	std::vector<MdRun> runs;
	try {
		MdCollectRuns(begin, end, runs);
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
	colors[2] = kMdInlineCodeColor;
	colors[3] = ImGui::GetColorU32(ImGuiCol_Text);
	colors[4] = ImGui::GetColorU32(ImGuiCol_TextDisabled);

	const float lineHeight = ImGui::GetTextLineHeight();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursor = ImGui::GetCursorScreenPos();
	const float startX = cursor.x;
	const float right = startX + ImGui::GetContentRegionAvail().x;
	float maxY = cursor.y + lineHeight;
	auto wrapUnitEnd = [](const char* unitBegin, const char* runEnd) noexcept {
		int charLen = 0;
		const unsigned c = MdDecodeUtf8(unitBegin, runEnd, &charLen);
		const char* unitEnd = unitBegin + charLen;
		if (c != ' ' && !MdIsWideCodepoint(c)) {
			while (unitEnd < runEnd) {
				int nextLen = 0;
				const unsigned next =
					MdDecodeUtf8(unitEnd, runEnd, &nextLen);
				if (next == ' ' || MdIsWideCodepoint(next))
					break;
				unitEnd += nextLen;
			}
		}
		return unitEnd;
	};

	for (const MdRun& run : runs) {
		ImFont* font = fonts[run.style];
		const bool codeBg = run.style == 2;
		const char* w = run.begin;
		if (codeBg) {
			while (w < run.end) {
				const char* firstEnd = wrapUnitEnd(w, run.end);
				const float firstWidth = font->CalcTextSizeA(
					font->FontSize, FLT_MAX, 0.0f, w, firstEnd).x;
				if (cursor.x + firstWidth > right && cursor.x > startX) {
					cursor.x = startX;
					cursor.y += lineHeight;
				}

				const char* segmentBegin = w;
				float segmentWidth = 0.0f;
				while (w < run.end) {
					const char* unitEnd = wrapUnitEnd(w, run.end);
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
					kMdInlineCodeBackground, 2.0f);
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
			const char* unitEnd = wrapUnitEnd(w, run.end);
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

// Reusable markdown renderer; instance state lives across frames so table and
// code-block buffers never reallocate during streaming.
struct HubMarkdownRenderer {
	ImFont* bold = nullptr;
	ImFont* code = nullptr;
	std::string normalizedText_;
	std::string codeBuffer_;
	std::vector<std::vector<std::string>> tableRows;
	std::vector<float> tableWidths_;
	int codeBlockIndex_ = 0;

	ImFont* FontOrDefault(ImFont* font) const noexcept {
		return font && font->IsLoaded() ? font : ImGui::GetFont();
	}

	void Render(const char* text) noexcept {
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

	void RenderBlockLines(const char* begin, const char* end) {
		bool inCodeFence = false;
		codeBuffer_.clear();

		// Wrap every text item at the window edge; code blocks use their own
		// child window (horizontal scroll, no wrap) and table cells push
		// their own wrap width, so neither is affected by this.
		ImGui::PushTextWrapPos(0.0f);
		const char* lineBegin = begin;
		while (lineBegin < end) {
			const char* lineEnd = MdFindChar(lineBegin, end, '\n');
			if (!lineEnd)
				lineEnd = end;
			const char* contentEnd = lineEnd;
			while (contentEnd > lineBegin &&
				(contentEnd[-1] == '\r' || contentEnd[-1] == ' '))
				--contentEnd;

			const bool isFence = contentEnd - lineBegin >= 3 &&
				lineBegin[0] == '`' && lineBegin[1] == '`' &&
				lineBegin[2] == '`';
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
			else if (MdIsBlankLine(lineBegin, contentEnd)) {
				ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.40f));
			}
			else {
				int leading = 0;
				const char* p = lineBegin;
				while (p < contentEnd && (*p == ' ' || *p == '\t')) {
					leading += *p == '\t' ? 4 : 1;
					++p;
				}
				const float indentPx =
					static_cast<float>((leading / 2 > 3 ? 3 : leading / 2) * 14);

				if (p < contentEnd && p[0] == '|') {
					tableRows.clear();
					const char* scan = lineBegin;
					while (scan < end && tableRows.size() < 64) {
						const char* scanEnd = MdFindChar(scan, end, '\n');
						if (!scanEnd)
							scanEnd = end;
						const char* cellEnd = scanEnd;
						while (cellEnd > scan &&
							(cellEnd[-1] == '\r' || cellEnd[-1] == ' '))
							--cellEnd;
						const char* cellStart = MdSkipSpaces(scan, cellEnd);
						if (cellStart >= cellEnd || cellStart[0] != '|')
							break;
						tableRows.emplace_back();
						if (!MdParseTableRow(cellStart, cellEnd,
							&tableRows.back())) {
							tableRows.pop_back();
							break;
						}
						scan = scanEnd < end ? scanEnd + 1 : end;
					}
					if (tableRows.size() >= 2 &&
						tableRows[0].size() == tableRows[1].size() &&
						tableRows[0].size() <= 8) {
						bool separatorOk = true;
						for (const std::string& cell : tableRows[1]) {
							if (!MdIsTableSeparatorCell(cell)) {
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
						MdIsHorizontalRule(p, contentEnd);
					const bool isBullet = contentEnd - p >= 2 &&
						(p[0] == '-' || p[0] == '*' || p[0] == '+') &&
						p[1] == ' ';
					const char* orderedContent = nullptr;
					const bool isOrdered = !isHr &&
						MdParseOrderedListMarker(p, contentEnd, &orderedContent);
					const bool isQuote = p[0] == '>';
					int headingLevel = 0;
					if (p < contentEnd && p[0] == '#') {
						const char* hashes = p;
						while (hashes < contentEnd && *hashes == '#' &&
							headingLevel < 6) {
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
							kAccent);
						ImGui::Bullet();
						ImGui::PopStyleColor();
						MdRenderInlineRuns(p + 2, contentEnd, bold, code);
					}
					else if (isOrdered) {
						ImGui::PushStyleColor(ImGuiCol_Text,
							kAccent);
						ImGui::TextUnformatted(p, orderedContent - 2);
						ImGui::PopStyleColor();
						ImGui::SameLine(0.0f, 3.0f);
						MdRenderInlineRuns(orderedContent, contentEnd, bold, code);
					}
					else if (isQuote) {
						const char* quoteText = MdSkipSpaces(p + 1, contentEnd);
						ImGui::Indent(10.0f);
						ImGui::PushStyleColor(ImGuiCol_Text,
							ImGui::ColorConvertU32ToFloat4(kMdQuoteColor));
						ImGui::TextUnformatted(quoteText, contentEnd);
						ImGui::PopStyleColor();
						const ImVec2 quoteMin = ImGui::GetItemRectMin();
						const ImVec2 quoteMax = ImGui::GetItemRectMax();
						ImGui::GetWindowDrawList()->AddRectFilled(
							ImVec2(quoteMin.x - 8.0f, quoteMin.y - 1.0f),
							ImVec2(quoteMin.x - 5.0f, quoteMax.y + 1.0f),
							kMdQuoteBarColor, 1.5f);
						ImGui::Unindent(10.0f);
					}
					else if (headingLevel > 0) {
						const char* headingText =
							MdSkipSpaces(p + headingLevel, contentEnd);
						ImGui::Dummy(ImVec2(0.0f,
							ImGui::GetTextLineHeight() * 0.20f));
						ImGui::PushStyleColor(ImGuiCol_Text,
							ImGui::ColorConvertU32ToFloat4(kMdHeadingColor));
						if (bold) {
							ImGui::PushFont(FontOrDefault(bold));
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
							const float lineY =
								ImGui::GetItemRectMax().y + 3.0f;
							const float right = headingMin.x +
								ImGui::GetContentRegionAvail().x;
							ImGui::GetWindowDrawList()->AddLine(
								ImVec2(headingMin.x, lineY),
								ImVec2(right, lineY), kMdHeadingUnderline,
								headingLevel == 1 ? 2.0f : 1.0f);
						}
						ImGui::Dummy(ImVec2(0.0f,
							ImGui::GetTextLineHeight() * 0.25f));
					}
					else {
						MdRenderInlineRuns(p, contentEnd, bold, code);
					}

					if (indentPx > 0.0f)
						ImGui::Unindent(indentPx);
				}
			}

			lineBegin = lineEnd < end ? lineEnd + 1 : end;
		}
		ImGui::PopTextWrapPos();

		if (inCodeFence && !codeBuffer_.empty())
			RenderCodeBlock(codeBuffer_.data(),
				codeBuffer_.data() + codeBuffer_.size());
	}

	void RenderCodeBlock(const char* begin, const char* end) {
		ImGui::PushStyleColor(ImGuiCol_ChildBg,
			ImGui::ColorConvertU32ToFloat4(kMdCodeBackground));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
		const float lineCount =
			static_cast<float>(std::count(begin, end, '\n')) + 1.0f;
		const float height = lineCount * ImGui::GetTextLineHeight() +
			ImGui::GetStyle().WindowPadding.y * 2.0f;
		ImGui::PushID(codeBlockIndex_++);
		if (ImGui::BeginChild("##codeBlock", ImVec2(0.0f, height),
			ImGuiChildFlags_Borders,
			ImGuiWindowFlags_HorizontalScrollbar)) {
			if (code) {
				ImGui::PushFont(FontOrDefault(code));
				ImGui::TextUnformatted(begin, end);
				ImGui::PopFont();
			}
			else {
				ImGui::TextUnformatted(begin, end);
			}
		}
		ImGui::EndChild();
		ImGui::PopID();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.35f));
	}

	void RenderMarkdownTable() {
		if (tableRows.empty())
			return;
		const size_t columnCount = tableRows[0].size();
		if (columnCount == 0)
			return;
		ImFont* headerBold = bold ? FontOrDefault(bold) : nullptr;
		constexpr float padX = 8.0f;
		constexpr float padY = 3.0f;

		tableWidths_.assign(columnCount, 0.0f);
		for (size_t row = 0; row < tableRows.size(); ++row) {
			const std::vector<std::string>& cells = tableRows[row];
			const size_t count =
				cells.size() < columnCount ? cells.size() : columnCount;
			for (size_t column = 0; column < count; ++column) {
				const std::string& cell = cells[column];
				float width;
				if (row == 0 && headerBold) {
					ImGui::PushFont(headerBold);
					width = ImGui::CalcTextSize(
						cell.data(), cell.data() + cell.size()).x;
					ImGui::PopFont();
				}
				else {
					width = ImGui::CalcTextSize(
						cell.data(), cell.data() + cell.size()).x;
				}
				tableWidths_[column] = (tableWidths_[column] > width)
					? tableWidths_[column]
					: width;
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
		for (size_t row = 0; row < tableRows.size(); ++row) {
			const std::vector<std::string>& cells = tableRows[row];
			float x = start.x;
			float rowHeight = ImGui::GetTextLineHeight() + padY * 2.0f;
			const bool isHeader = row == 0;
			for (size_t column = 0; column < columnCount; ++column) {
				ImGui::SetCursorScreenPos(ImVec2(x + padX, y + padY));
				ImGui::PushTextWrapPos(x + tableWidths_[column] - padX);
				const bool pushedFont = isHeader && headerBold != nullptr;
				if (pushedFont)
					ImGui::PushFont(headerBold);
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
				isHeader ? kMdTableHeaderLine : kMdTableLine,
				isHeader ? 2.0f : 1.0f);
			y += rowHeight;
		}
		ImGui::SetCursorScreenPos(ImVec2(start.x, y));
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.35f));
	}
};

} // namespace

App::~App() {
	aiClient_.Cancel();
}

int App::Run(HINSTANCE instance, int showCommand) noexcept {
	instance_ = instance;
	Logger::Initialize();
	Logger::Write(LogLevel::Info, "CoolHelperHub starting");
	const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
		Logger::Write(LogLevel::Error, "COM initialization failed");
		Logger::Shutdown();
		return 1;
	}

	LoadSettings();
	if (!CreateMainWindow(instance) || !CreateDevice()) {
		Logger::Write(LogLevel::Error, "Main window or D3D11 initialization failed");
		DestroyDevice();
		if (SUCCEEDED(comResult)) CoUninitialize();
		Logger::Shutdown();
		return 1;
	}
	InitializeImGui();
	if (imguiReady_) {
		// The font atlas build is the slowest part of startup; do it while
		// the window is still hidden so no blank frame reaches the screen.
		if (!BuildAndUploadHubFonts("startup")) {
			ShutdownImGui();
			DestroyDevice();
			DestroyWindow(window_);
			window_ = nullptr;
			if (SUCCEEDED(comResult)) CoUninitialize();
			Logger::Shutdown();
			return 1;
		}
	}
	answerSink_.SetWindow(window_);
	answerTee_.Add(&answerSink_);
	answerTee_.Add(&overlaySink_);
	AddTrayIcon();

	std::string privilegeError;
	if (!EnableDebugPrivilege(privilegeError))
		Logger::Write(LogLevel::Warning, "SeDebugPrivilege unavailable; DWM injection will fail");
	std::string ipcError;
	if (!overlaySink_.Start(ipcError))
		Logger::Write(LogLevel::Warning,
			("Overlay IPC sink not started: " + ipcError).c_str());
	if (!overlayControl_.Start(ipcError))
		Logger::Write(LogLevel::Warning,
			("Overlay control channel not started: " + ipcError).c_str());
	CopyField(overlayDllPathField_, settings_.overlayDllPath);

	std::string hotkeyError;
	if (!ApplyCaptureHotkey(settings_.captureHotkey, hotkeyError))
		lastError_ = hotkeyError;
	std::string overlayHotkeyError;
	if (!ApplyOverlayHotkey(settings_.overlayToggleHotkey, overlayHotkeyError))
		Logger::Write(LogLevel::Warning, overlayHotkeyError.c_str());
	std::string scrollHotkeyError;
	if (!ApplyOverlayScrollHotkey(
		settings_.overlayScrollDownHotkey, true, scrollHotkeyError))
		Logger::Write(LogLevel::Warning, scrollHotkeyError.c_str());
	if (!ApplyOverlayScrollHotkey(
		settings_.overlayScrollUpHotkey, false, scrollHotkeyError))
		Logger::Write(LogLevel::Warning, scrollHotkeyError.c_str());

	// Everything is ready; only now make the window visible.
	ShowWindow(window_, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
	UpdateWindow(window_);

	MSG message = {};
	while (running_) {
		overlayControl_.KeepAlive();
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			if (message.message == WM_QUIT) {
				running_ = false;
				break;
			}
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		if (!running_)
			break;
		if (IsWindowVisible(window_)) {
			RenderFrame();
			Sleep(8);
		}
		else {
			MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT,
				MWMO_INPUTAVAILABLE);
		}
	}

	Logger::Write(LogLevel::Info, "CoolHelperHub shutting down");
	aiClient_.Cancel();
	UnregisterOverlayScrollHotkeys();
	UnregisterOverlayHotkey();
	UnregisterCaptureHotkey();
	RemoveTrayIcon();
	answerSink_.SetWindow(nullptr);
	overlayControl_.Stop();
	overlaySink_.Stop();
	ShutdownImGui();
	DestroyDevice();
	if (window_ && IsWindow(window_))
		DestroyWindow(window_);
	window_ = nullptr;
	if (SUCCEEDED(comResult))
		CoUninitialize();
	Logger::Shutdown();
	return 0;
}

bool App::CreateMainWindow(HINSTANCE instance) noexcept {
	WNDCLASSEXW windowClass = { sizeof(windowClass) };
	windowClass.style = CS_CLASSDC;
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.hIcon = static_cast<HICON>(LoadImageW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
	windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	// Dark brush matching the theme so the pre-first-frame window is never
	// the default white.
	windowClass.hbrBackground = CreateSolidBrush(RGB(14, 17, 22));
	windowClass.lpszClassName = kMainWindowClass;
	if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;

	// Created hidden; Run() shows it only after the font atlas and the first
	// frame are ready.
	window_ = CreateWindowExW(0, kMainWindowClass, L"CoolHelperHub 面试助手",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720,
		nullptr, nullptr, instance, this);
	return window_ != nullptr;
}

bool App::CreateDevice() noexcept {
	DXGI_SWAP_CHAIN_DESC description = {};
	description.BufferCount = 2;
	description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	description.OutputWindow = window_;
	description.SampleDesc.Count = 1;
	description.Windowed = TRUE;
	description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	description.Flags = 0;

	D3D_FEATURE_LEVEL featureLevel = {};
	const D3D_FEATURE_LEVEL requested[] = {
		D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
	};
	HRESULT result = D3D11CreateDeviceAndSwapChain(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested,
		static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
		&description, &swapChain_, &device_, &featureLevel, &context_);
	if (FAILED(result)) {
		Logger::Write(LogLevel::Warning,
			"Hardware D3D11 unavailable; falling back to WARP");
		result = D3D11CreateDeviceAndSwapChain(
			nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, requested,
			static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
			&description, &swapChain_, &device_, &featureLevel, &context_);
		if (FAILED(result))
			return false;
	}
	CreateRenderTarget();
	return renderTarget_ != nullptr;
}

void App::DestroyDevice() noexcept {
	DestroyRenderTarget();
	swapChain_.Reset();
	context_.Reset();
	device_.Reset();
}

void App::CreateRenderTarget() noexcept {
	if (!swapChain_ || !device_)
		return;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
	if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
		device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
}

void App::DestroyRenderTarget() noexcept {
	renderTarget_.Reset();
}

void App::InitializeImGui() noexcept {
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	// Start with ImGui's common Simplified Chinese set and the user's current
	// prompt text. Baking the full 21k CJK set into all three fonts can exceed
	// D3D11's maximum texture height; uncommon answer glyphs are added on demand.
	// The range storage must persist because the atlas only stores its pointer.
	ImFontGlyphRangesBuilder glyphBuilder;
	glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
	glyphBuilder.AddText(kStaticUiGlyphSeed);
	glyphBuilder.AddText(settings_.systemPrompt.c_str());
	glyphBuilder.AddText(settings_.userPrompt.c_str());
	glyphBuilder.AddText(kDefaultInterviewSystemPrompt);
	glyphBuilder.AddText(kDefaultInterviewUserPrompt);
	glyphBuilder.AddText(kAlgorithmInterviewSystemPrompt);
	glyphBuilder.AddText(kAlgorithmInterviewUserPrompt);
	ImVector<ImWchar> staticGlyphRanges;
	glyphBuilder.BuildRanges(&staticGlyphRanges);
	extraGlyphRanges_.assign(staticGlyphRanges.Data,
		staticGlyphRanges.Data + staticGlyphRanges.Size);
	ReloadHubFonts();
	ApplyHubTheme();
	ImGui_ImplWin32_Init(window_);
	ImGui_ImplDX11_Init(device_.Get(), context_.Get());
	imguiReady_ = true;
}

void App::ReloadHubFonts() noexcept {
	ImGuiIO& io = ImGui::GetIO();
	// Rasterize at the display's native DPI with vertical oversampling: the
	// defaults (OversampleV=1) blur text that lands on fractional rows.
	const UINT dpi = GetDpiForWindow(window_);
	const float dpiScale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
	ImFontConfig bodyConfig = {};
	bodyConfig.OversampleH = 2;
	bodyConfig.OversampleV = 2;
	// The common Chinese range covers UI text while extraGlyphRanges_ carries
	// current prompts and uncommon characters discovered in streamed answers.
	const ImWchar* cjkRanges = extraGlyphRanges_.empty()
		? io.Fonts->GetGlyphRangesChineseSimplifiedCommon()
		: reinterpret_cast<const ImWchar*>(extraGlyphRanges_.data());
	fontRegular_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",
		std::round(18.0f * dpiScale), &bodyConfig, cjkRanges);
	if (fontRegular_)
		MergeSymbolFont(io.Fonts, std::round(18.0f * dpiScale));
	// The bold face is also used by the markdown renderer for headings and
	// **runs**, so it carries the same CJK coverage.
	ImFontConfig titleConfig = {};
	titleConfig.OversampleH = 2;
	titleConfig.OversampleV = 2;
	fontTitle_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyhbd.ttc",
		std::round(19.0f * dpiScale), &titleConfig, cjkRanges);
	if (fontTitle_)
		MergeSymbolFont(io.Fonts, std::round(19.0f * dpiScale));
	ImFontConfig codeConfig = {};
	codeConfig.OversampleH = 2;
	codeConfig.OversampleV = 2;
	fontCode_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf",
		std::round(16.0f * dpiScale), &codeConfig,
		io.Fonts->GetGlyphRangesDefault());
	if (fontCode_) {
		// Consolas has no CJK glyphs. Merge YaHei so Chinese comments inside
		// Markdown code blocks do not fall back to '?'.
		ImFontConfig codeCjkConfig = {};
		codeCjkConfig.MergeMode = true;
		codeCjkConfig.OversampleH = 2;
		codeCjkConfig.OversampleV = 2;
		io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",
			std::round(16.0f * dpiScale), &codeCjkConfig, cjkRanges);
		MergeSymbolFont(io.Fonts, std::round(16.0f * dpiScale));
	}
	if (!fontRegular_)
		fontRegular_ = io.Fonts->AddFontDefault();
	if (!fontTitle_ || !fontTitle_->IsLoaded())
		fontTitle_ = fontRegular_;
	if (!fontCode_ || !fontCode_->IsLoaded())
		fontCode_ = fontRegular_;
	ApplyHubTheme();
}

void App::ScanMissingAnswerGlyphs() noexcept {
	if (!fontRegular_)
		return;
	if (answer_.size() < glyphScannedUpTo_) {
		glyphScannedUpTo_ = 0; // answer was cleared or replaced
		pendingMissingGlyphs_.clear();
	}

	if (answer_.size() != glyphScannedUpTo_) {
		const char* text = answer_.c_str();
		const char* end = text + answer_.size();
		const char* cursor = text + glyphScannedUpTo_;
		while (cursor < end) {
			unsigned codepoint = static_cast<unsigned char>(*cursor);
			int length = 1;
			if (codepoint >= 0x80) {
				if ((codepoint & 0xE0) == 0xC0 && cursor + 1 < end) {
					codepoint = ((codepoint & 0x1F) << 6) | (cursor[1] & 0x3F);
					length = 2;
				}
				else if ((codepoint & 0xF0) == 0xE0 && cursor + 2 < end) {
					codepoint = ((codepoint & 0x0F) << 12) |
						((cursor[1] & 0x3F) << 6) | (cursor[2] & 0x3F);
					length = 3;
				}
				else if ((codepoint & 0xF8) == 0xF0 && cursor + 3 < end) {
					codepoint = ((codepoint & 0x07) << 18) |
						((cursor[1] & 0x3F) << 12) |
						((cursor[2] & 0x3F) << 6) | (cursor[3] & 0x3F);
					length = 4;
				}
				else {
					break; // Keep the partial UTF-8 sequence for the next frame.
				}
			}
			if (codepoint >= 0x80 && codepoint <= 0xFFFF &&
				fontRegular_->FindGlyphNoFallback(
					static_cast<ImWchar>(codepoint)) == nullptr) {
				pendingMissingGlyphs_.append(
					cursor, static_cast<size_t>(length));
			}
			cursor += length;
		}
		glyphScannedUpTo_ = static_cast<std::uint64_t>(cursor - text);
	}

	if (pendingMissingGlyphs_.empty())
		return;
	const ULONGLONG now = GetTickCount64();
	if (lastGlyphRebuildTick_ != 0 && now - lastGlyphRebuildTick_ < 1500)
		return;
	lastGlyphRebuildTick_ = now;

	ImGuiIO& io = ImGui::GetIO();
	ImFontGlyphRangesBuilder builder;
	builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
	if (!extraGlyphRanges_.empty())
		builder.AddRanges(reinterpret_cast<const ImWchar*>(
			extraGlyphRanges_.data()));
	builder.AddText(pendingMissingGlyphs_.c_str());
	pendingMissingGlyphs_.clear();
	ImVector<ImWchar> built;
	builder.BuildRanges(&built);
	const std::vector<std::uint16_t> previousRanges = extraGlyphRanges_;
	extraGlyphRanges_.assign(built.Data, built.Data + built.Size);
	ImGui_ImplDX11_InvalidateDeviceObjects();
	io.Fonts->Clear();
	ReloadHubFonts();
	if (!BuildAndUploadHubFonts("answer glyph update")) {
		Logger::Write(LogLevel::Warning,
			"Extended glyph atlas rejected; restoring the previous atlas");
		extraGlyphRanges_ = previousRanges;
		io.Fonts->Clear();
		ReloadHubFonts();
		if (!BuildAndUploadHubFonts("glyph fallback")) {
			Logger::Write(LogLevel::Error,
				"Font atlas fallback failed; rendering has been stopped");
			running_ = false;
			PostQuitMessage(1);
			return;
		}
	}
	io.FontDefault = nullptr;
	Logger::Write(LogLevel::Info,
		"Font atlas rebuilt with extended glyph ranges");
}

void App::ShutdownImGui() noexcept {
	if (!imguiReady_)
		return;
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	imguiReady_ = false;
}

void App::RenderFrame() noexcept {
	if (!imguiReady_ || !renderTarget_)
		return;
	DrainAnswerEvents();
	ScanMissingAnswerGlyphs();
	if (!running_)
		return;
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
	ImGui::Begin("CoolHelperHubRoot", nullptr, flags);

	if (fontTitle_)
		ImGui::PushFont(fontTitle_);
	ImGui::TextColored(kAccent, "CoolHelperHub");
	if (fontTitle_)
		ImGui::PopFont();
	ImGui::SameLine();
	ImGui::TextDisabled("面试截图助手");
	ImGui::SameLine();
	const std::string hotkeyText = HotkeyText(settings_.captureHotkey);
	const std::string overlayHotkeyText =
		HotkeyText(settings_.overlayToggleHotkey);
	RightDisabledText(hotkeyText + " 截图  ·  " + overlayHotkeyText +
		" 隐藏/显示覆盖层");
	ImGui::Separator();
	ImGui::Spacing();

	if (ImGui::BeginTabBar("MainTabs",
		ImGuiTabBarFlags_NoTabListScrollingButtons)) {
		if (ImGui::BeginTabItem("答案")) {
			RenderAnswerPage();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("DWM 覆盖层")) {
			RenderDwmPage();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("设置")) {
			RenderSettingsPage();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();

	ImGui::Render();
	const float clearColor[4] = { 0.055f, 0.065f, 0.085f, 1.0f };
	ID3D11RenderTargetView* target = renderTarget_.Get();
	context_->OMSetRenderTargets(1, &target, nullptr);
	context_->ClearRenderTargetView(target, clearColor);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	const HRESULT present = swapChain_->Present(1, 0);
	if (present == DXGI_STATUS_OCCLUDED)
		Sleep(20);
}

void App::RenderAnswerPage() noexcept {
	StatusChip(StateText(), StateColor(requestState_));
	if (IsActiveRequestState(requestState_) && requestStartedTick_ != 0) {
		ImGui::SameLine();
		const ULONGLONG elapsedSeconds =
			(GetTickCount64() - requestStartedTick_) / 1000;
		ChipAlignedText("已等待 " + std::to_string(elapsedSeconds) + " 秒");
	}
	if (screenshotWidth_ > 0) {
		ImGui::SameLine();
		ChipAlignedText("截图 " + std::to_string(screenshotWidth_) +
			" × " + std::to_string(screenshotHeight_));
	}
	ImGui::Spacing();

	if (AccentButton("截图并提问"))
		TriggerCapture();
	ImGui::SameLine();
	ImGui::BeginDisabled(!IsActiveRequestState(requestState_));
	if (SecondaryButton("停止"))
		aiClient_.Cancel();
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (SecondaryButton("清空")) {
		aiClient_.Cancel();
		answerSink_.Drain();
		answer_.clear();
		progressText_.clear();
		lastError_.clear();
		requestStartedTick_ = 0;
		requestState_ = RequestState::Idle;
		activeRequestId_ = nextRequestId_++;
		lastSequence_ = 0;
		overlaySink_.SetRequestIdFilter(activeRequestId_);
		answerTee_.Publish({ activeRequestId_, 1,
			AnswerEventType::Cleared, {} });
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(answer_.empty());
	if (SecondaryButton("复制答案"))
		ImGui::SetClipboardText(answer_.c_str());
	ImGui::EndDisabled();

	if (!lastError_.empty()) {
		ImGui::Spacing();
		NoticeBox("##answerError", lastError_, kError);
		ImGui::Spacing();
	}
	ImGui::Separator();
	if (requestState_ == RequestState::Thinking && !progressText_.empty()) {
		ImGui::TextDisabled("模型推理：%s", progressText_.c_str());
		ImGui::Separator();
	}
	else if (requestState_ == RequestState::Connecting) {
		ImGui::TextDisabled("正在连接并上传截图…");
		ImGui::Separator();
	}
	else if (requestState_ == RequestState::Waiting) {
		ImGui::TextDisabled("服务已连接，等待模型响应…");
		ImGui::Separator();
	}

	// Markdown answer view: wraps to the window width, streams with
	// autoscroll, and copies remain available through the button above.
	static HubMarkdownRenderer markdown;
	markdown.bold = fontTitle_;
	markdown.code = fontCode_;
	ImGui::BeginChild("answerScroll", ImVec2(0.0f, 0.0f), 0, 0);
	const bool streaming = IsActiveRequestState(requestState_);
	const bool atBottom = ImGui::GetScrollY() + ImGui::GetWindowHeight() >=
		ImGui::GetScrollMaxY() - 4.0f;
	if (!answer_.empty()) {
		markdown.Render(answer_.c_str());
	}
	else if (lastError_.empty()) {
		ImGui::TextDisabled(
			"截图提问后，AI 回答将在这里按 Markdown 格式实时显示。");
	}
	if (streaming && atBottom)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

void App::RenderSettingsPage() noexcept {
	ImGui::BeginChild("settingsScroll", ImVec2(0.0f, 0.0f), 0, 0);

	ImGui::SeparatorText("接口配置");
	if (ImGui::BeginChild("##apiCard", ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
		DisabledTextWrapped(
			"API Key 使用 Windows DPAPI 加密，仅能由当前 Windows 用户解密。");
		ImGui::TextUnformatted("API Base URL");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##apiBaseUrl", apiBaseUrlField_.data(),
			apiBaseUrlField_.size());
		ImGui::TextUnformatted("API Key");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##apiKey", apiKeyField_.data(), apiKeyField_.size(),
			ImGuiInputTextFlags_Password);
		ImGui::TextUnformatted("Model");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##model", modelField_.data(), modelField_.size());
	}
	ImGui::EndChild();
	ImGui::Spacing();

	ImGui::SeparatorText("提示词");
	if (ImGui::BeginChild("##promptCard", ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
		ImGui::TextUnformatted("System Prompt");
		ImGui::InputTextMultiline("##systemPrompt", systemPromptField_.data(),
			systemPromptField_.size(), ImVec2(-FLT_MIN,
				ImGui::GetTextLineHeightWithSpacing() * 8.0f));
		ImGui::TextUnformatted("截图问题");
		ImGui::InputTextMultiline("##userPrompt", userPromptField_.data(),
			userPromptField_.size(), ImVec2(-FLT_MIN,
				ImGui::GetTextLineHeightWithSpacing() * 6.0f));
		ImGui::TextUnformatted("提示词预设");
		if (SecondaryButton("综合面试")) {
			CopyField(systemPromptField_, kDefaultInterviewSystemPrompt);
			CopyField(userPromptField_, kDefaultInterviewUserPrompt);
			settingsStatus_ = "已切换为综合面试提示词，点击“保存设置”后生效";
		}
		ImGui::SameLine();
		if (SecondaryButton("算法题")) {
			CopyField(systemPromptField_, kAlgorithmInterviewSystemPrompt);
			CopyField(userPromptField_, kAlgorithmInterviewUserPrompt);
			settingsStatus_ = "已切换为算法题提示词，点击“保存设置”后生效";
		}
	}
	ImGui::EndChild();
	ImGui::Spacing();

	ImGui::SeparatorText("全局快捷键");
	if (ImGui::BeginChild("##hotkeyCard", ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
		const auto hotkeyRow = [](const char* id, const char* label,
			CaptureHotkeySettings& hotkey) {
			ImGui::TextUnformatted(label);
			ImGui::PushID(id);
			// Keep the explanatory text below the controls.  The previous single
			// fixed-width row clipped it at smaller resolutions and high DPI.
			ImGui::Checkbox("Ctrl", &hotkey.control);
			SameLineIfFits(CheckboxWidth("Alt"));
			ImGui::Checkbox("Alt", &hotkey.alt);
			SameLineIfFits(CheckboxWidth("Shift"));
			ImGui::Checkbox("Shift", &hotkey.shift);
			SameLineIfFits(CheckboxWidth("Win"));
			ImGui::Checkbox("Win", &hotkey.windows);
			SameLineIfFits(90.0f);
			ImGui::SetNextItemWidth(
				std::min(90.0f, ImGui::GetContentRegionAvail().x));
			if (ImGui::BeginCombo("##key", HotkeyKeyName(hotkey.virtualKey))) {
				for (const auto& option : kHotkeyOptions) {
					const bool selected = option.virtualKey ==
						hotkey.virtualKey;
					if (ImGui::Selectable(option.name, selected))
						hotkey.virtualKey = option.virtualKey;
					if (selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
			ImGui::Spacing();
		};
		hotkeyRow("captureHotkey", "截图并提问", captureHotkeyField_);
		hotkeyRow("overlayHotkey", "切换覆盖层显示", overlayHotkeyField_);
		hotkeyRow("overlayScrollDownHotkey", "覆盖层答案向下滚动",
			overlayScrollDownHotkeyField_);
		hotkeyRow("overlayScrollUpHotkey", "覆盖层答案向上滚动",
			overlayScrollUpHotkeyField_);
		ImGui::TextDisabled("修改快捷键后，点击下方“保存设置”使其生效。");
	}
	ImGui::EndChild();
	ImGui::Spacing();
	ImGui::Separator();
	if (AccentButton("保存设置"))
		SaveSettingsFromFields();
	if (!settingsStatus_.empty()) {
		const ImVec4 statusColor =
			settingsStatus_.find("成功") != std::string::npos ||
			settingsStatus_.find("已切换") != std::string::npos
			? kSuccess
			: kWarning;
		ColoredTextWrapped(settingsStatus_, statusColor);
	}
	const std::string path = WideToUtf8(settingsStore_.Path().wstring());
	DisabledTextWrapped("配置文件：" + path);
	ImGui::EndChild();
}

std::wstring App::ResolveOverlayDllPath() const noexcept {
	if (!settings_.overlayDllPath.empty()) {
		const std::wstring configured = Utf8ToWide(settings_.overlayDllPath);
		if (!configured.empty())
			return configured;
	}
	wchar_t exePath[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
		return L"DwmCfgOverlayLab.dll";
	std::filesystem::path exeFile(exePath);
	exeFile.replace_filename(L"DwmCfgOverlayLab.dll");
	return exeFile.wstring();
}

void App::RefreshDwmStatus() noexcept {
	dwmLastRefreshTick_ = GetTickCount64();
	DwmProcessInfo info = {};
	std::string error;
	if (!FindDwmProcess(&info, error)) {
		dwmProcessId_ = 0;
		dwmInjected_ = false;
		return;
	}
	dwmProcessId_ = info.processId;
	bool loaded = false;
	if (IsDllLoadedInProcess(dwmProcessId_, ResolveOverlayDllPath(),
		&loaded, error)) {
		dwmInjected_ = loaded;
	}
}

bool App::InjectOverlayDll() noexcept {
	std::string privilegeError;
	EnableDebugPrivilege(privilegeError);

	const std::wstring dllPath = ResolveOverlayDllPath();
	if (!std::filesystem::exists(dllPath)) {
		dwmError_ = "找不到 DLL：" + WideToUtf8(dllPath);
		return false;
	}
	bool loaded = false;
	std::string error;
	if (IsDllLoadedInProcess(dwmProcessId_, dllPath, &loaded, error) && loaded) {
		dwmError_.clear();
		dwmStatus_ = "DLL 已经在 dwm 进程中";
		return true;
	}
	if (!InjectDllIntoProcess(dwmProcessId_, dllPath, error)) {
		dwmError_ = error;
		Logger::Write(LogLevel::Error, "DWM DLL injection failed");
		return false;
	}
	dwmError_.clear();
	dwmStatus_ = "注入成功，等待 DWM 覆盖层渲染连接";
	Logger::Write(LogLevel::Info, "DWM DLL injected");
	return true;
}

bool App::EjectOverlayDll() noexcept {
	std::string error;
	if (!EjectDllFromProcess(dwmProcessId_, ResolveOverlayDllPath(), error)) {
		dwmError_ = error;
		Logger::Write(LogLevel::Error, "DWM DLL ejection failed");
		return false;
	}
	dwmError_.clear();
	dwmStatus_ = "已安全卸载";
	Logger::Write(LogLevel::Info, "DWM DLL ejected");
	return true;
}

void App::ToggleOverlayVisible() noexcept {
	if (!overlayControl_.IsRunning())
		return;
	if (!overlayControl_.IsDllConnected()) {
		dwmStatus_ = "DWM 覆盖层未连接，切换指令未发送";
		return;
	}
	// The DLL reports its actual state; unknown counts as visible so the
	// first hotkey press hides it.
	const bool show = overlayControl_.QueryOverlayVisible() == 0;
	if (overlayControl_.SendSetOverlayVisible(show)) {
		dwmStatus_ = show ? "已发送：显示覆盖层" : "已发送：隐藏覆盖层";
		dwmError_.clear();
		Logger::Write(LogLevel::Info,
			show ? "Overlay show command sent" : "Overlay hide command sent");
	}
	else {
		dwmStatus_.clear();
		dwmError_ = "覆盖层切换指令发送失败（控制通道积压或未连接）";
		Logger::Write(LogLevel::Error, "Overlay toggle command send failed");
	}
}

void App::ResetOverlayState() noexcept {
	dwmStatus_.clear();
	dwmError_.clear();
}

void App::RenderDwmPage() noexcept {
	const ULONGLONG now = GetTickCount64();
	if (!dwmBusy_ && now - dwmLastRefreshTick_ >= 1000)
		RefreshDwmStatus();

	ImGui::BeginChild("dwmPageScroll", ImVec2(0.0f, 0.0f), 0, 0);
	DisabledTextWrapped(
		"将 DwmCfgOverlayLab.dll 注入 dwm.exe，AI 回答会通过共享内存实时推送到"
		"桌面覆盖层显示。注入与卸载都需要管理员权限；程序默认以管理员启动。");
	ImGui::Spacing();

	ImGui::SeparatorText("运行状态");
	if (ImGui::BeginChild("##dwmStatus", ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
		if (ImGui::BeginTable("##dwmStatusTable", 2,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
			ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
				ImGui::CalcTextSize("切换快捷键").x + 24.0f);
			ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
			const auto statusRow = [](const char* label, const char* value,
				const ImVec4& color) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextDisabled("%s", label);
				ImGui::TableSetColumnIndex(1);
				ColoredTextWrapped(value, color);
			};
			const std::string processId = dwmProcessId_
				? std::to_string(dwmProcessId_) : "未找到";
			statusRow("DWM 进程", processId.c_str(),
				dwmProcessId_ ? ImVec4(0.922f, 0.940f, 0.970f, 1.0f) : kFaint);
			statusRow("注入状态", dwmInjected_ ? "已注入" : "未注入",
				dwmInjected_ ? kSuccess : kFaint);
			statusRow("IPC 状态", overlaySink_.DescribeStatus(),
				overlaySink_.IsConnected() ? kSuccess : kWarning);
			const int overlayVisible = overlayControl_.QueryOverlayVisible();
			statusRow("覆盖层显示",
				overlayVisible == 1 ? "显示中"
				: (overlayVisible == 0 ? "已隐藏" : "未知"),
				overlayVisible == 1 ? kSuccess
				: (overlayVisible == 0 ? kFaint : kWarning));
			const std::string overlayHotkeyText =
				HotkeyText(settings_.overlayToggleHotkey);
			statusRow("切换快捷键", overlayHotkeyText.c_str(), kFaint);
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();
	ImGui::Spacing();

	ImGui::SeparatorText("控制");
	{
		ImGui::BeginDisabled(dwmBusy_ || !dwmProcessId_ || dwmInjected_);
		if (AccentButton(dwmInjected_ ? "已注入" : "注入 DLL")) {
			dwmBusy_ = true;
			if (dwmProcessId_)
				InjectOverlayDll();
			RefreshDwmStatus();
			dwmBusy_ = false;
		}
		ImGui::EndDisabled();
		SameLineForButtonIfFits("卸载 DLL");
		ImGui::BeginDisabled(dwmBusy_ || !dwmProcessId_ || !dwmInjected_);
		if (DangerButton("卸载 DLL")) {
			dwmBusy_ = true;
			if (dwmProcessId_ && dwmInjected_)
				EjectOverlayDll();
			RefreshDwmStatus();
			dwmBusy_ = false;
		}
		ImGui::EndDisabled();
		const char* visibilityLabel = overlayControl_.QueryOverlayVisible() == 0
			? "显示覆盖层" : "隐藏覆盖层";
		SameLineForButtonIfFits(visibilityLabel);
		ImGui::BeginDisabled(!overlayControl_.IsDllConnected());
		if (SecondaryButton(visibilityLabel))
			ToggleOverlayVisible();
		ImGui::EndDisabled();
		SameLineForButtonIfFits("刷新状态");
		if (SecondaryButton("刷新状态"))
			RefreshDwmStatus();
	}
	ImGui::Spacing();

	ImGui::SeparatorText("DLL 路径");
	const float pathRowWidth = ImGui::GetContentRegionAvail().x;
	const float pathButtonsWidth = ActionButtonWidth("保存路径") +
		ActionButtonWidth("恢复默认") + ImGui::GetStyle().ItemSpacing.x * 2.0f;
	const bool inlinePathActions = pathRowWidth >= pathButtonsWidth + 240.0f;
	ImGui::SetNextItemWidth(inlinePathActions
		? pathRowWidth - pathButtonsWidth : -FLT_MIN);
	ImGui::InputText("##overlayDllPath",
		overlayDllPathField_.data(), overlayDllPathField_.size());
	if (inlinePathActions)
		ImGui::SameLine();
	if (SecondaryButton("保存路径")) {
		settings_.overlayDllPath = overlayDllPathField_.data();
		std::string error;
		if (settingsStore_.Save(settings_, error)) {
			dwmStatus_ = "DLL 路径已保存";
			dwmError_.clear();
			Logger::Write(LogLevel::Info, "Overlay DLL path saved");
		}
		else {
			dwmError_ = error;
		}
		RefreshDwmStatus();
	}
	SameLineForButtonIfFits("恢复默认");
	if (SecondaryButton("恢复默认")) {
		settings_.overlayDllPath.clear();
		overlayDllPathField_[0] = '\0';
		std::string error;
		if (settingsStore_.Save(settings_, error))
			dwmStatus_ = "已恢复默认 DLL 路径（程序目录）";
		else
			dwmError_ = error;
		RefreshDwmStatus();
	}
	DisabledTextWrapped("当前使用：" +
		WideToUtf8(ResolveOverlayDllPath()));
	ImGui::Spacing();

	if (!dwmError_.empty())
		NoticeBox("##dwmError", dwmError_, kError);
	else if (!dwmStatus_.empty()) {
		const bool positive =
			dwmStatus_.find("成功") != std::string::npos ||
			dwmStatus_.find("已发送") != std::string::npos ||
			dwmStatus_.find("已保存") != std::string::npos;
		NoticeBox("##dwmStatus", dwmStatus_,
			positive ? kSuccess : kAccent);
	}

	ImGui::Spacing();
	DisabledTextWrapped(
		"卸载时会先远程调用 DLL 的 ShutdownDwmOverlay 恢复被修改的 DWM 调用点，"
		"确认安全后才执行 FreeLibrary；若钩子无法恢复，DLL 会保留加载以保护系统稳定。");
	ImGui::EndChild();
}

void App::LoadSettings() noexcept {
	std::string error;
	if (!settingsStore_.Load(settings_, error)) {
		lastError_ = error;
		Logger::Write(LogLevel::Warning, "Settings were not loaded; defaults are active");
	}
	CopySettingsToFields();
}

void App::CopySettingsToFields() noexcept {
	CopyField(apiBaseUrlField_, settings_.apiBaseUrl);
	CopyField(apiKeyField_, settings_.apiKey);
	CopyField(modelField_, settings_.model);
	CopyField(systemPromptField_, settings_.systemPrompt);
	CopyField(userPromptField_, settings_.userPrompt);
	captureHotkeyField_ = settings_.captureHotkey;
	overlayHotkeyField_ = settings_.overlayToggleHotkey;
	overlayScrollDownHotkeyField_ = settings_.overlayScrollDownHotkey;
	overlayScrollUpHotkeyField_ = settings_.overlayScrollUpHotkey;
}

void App::SaveSettingsFromFields() noexcept {
	AppSettings updated = settings_;
	updated.apiBaseUrl = apiBaseUrlField_.data();
	updated.apiKey = apiKeyField_.data();
	updated.model = modelField_.data();
	updated.systemPrompt = systemPromptField_.data();
	updated.userPrompt = userPromptField_.data();
	updated.captureHotkey = captureHotkeyField_;
	updated.overlayToggleHotkey = overlayHotkeyField_;
	updated.overlayScrollDownHotkey = overlayScrollDownHotkeyField_;
	updated.overlayScrollUpHotkey = overlayScrollUpHotkeyField_;
	const AppSettings previous = settings_;
	const auto restoreHotkeys = [this, &previous]() {
		std::string ignored;
		ApplyCaptureHotkey(previous.captureHotkey, ignored);
		ApplyOverlayHotkey(previous.overlayToggleHotkey, ignored);
		ApplyOverlayScrollHotkey(
			previous.overlayScrollDownHotkey, true, ignored);
		ApplyOverlayScrollHotkey(
			previous.overlayScrollUpHotkey, false, ignored);
	};
	std::string error;
	if (!ApplyCaptureHotkey(updated.captureHotkey, error)) {
		settingsStatus_ = error;
		return;
	}
	if (!ApplyOverlayHotkey(updated.overlayToggleHotkey, error)) {
		restoreHotkeys();
		settingsStatus_ = error;
		return;
	}
	if (!ApplyOverlayScrollHotkey(
		updated.overlayScrollDownHotkey, true, error)) {
		restoreHotkeys();
		settingsStatus_ = error;
		return;
	}
	if (!ApplyOverlayScrollHotkey(
		updated.overlayScrollUpHotkey, false, error)) {
		restoreHotkeys();
		settingsStatus_ = error;
		return;
	}

	if (settingsStore_.Save(updated, error)) {
		settings_ = std::move(updated);
		settingsStatus_ = "保存成功，快捷键已生效";
		Logger::Write(LogLevel::Info, "Settings saved");
	}
	else {
		restoreHotkeys();
		settingsStatus_ = error;
		Logger::Write(LogLevel::Error, "Settings save failed");
	}
}

bool App::ApplyCaptureHotkey(
	const CaptureHotkeySettings& hotkey, std::string& error) noexcept {
	if (IsOrdinaryUnmodifiedKey(hotkey)) {
		error = "字母或数字快捷键至少需要选择 Ctrl、Alt、Shift 或 Win";
		return false;
	}
	if (hotkeyRegistered_ &&
		hotkey.control == registeredHotkey_.control &&
		hotkey.alt == registeredHotkey_.alt &&
		hotkey.shift == registeredHotkey_.shift &&
		hotkey.windows == registeredHotkey_.windows &&
		hotkey.virtualKey == registeredHotkey_.virtualKey)
		return true;

	const bool hadPrevious = hotkeyRegistered_;
	const CaptureHotkeySettings previous = registeredHotkey_;
	if (hadPrevious) {
		UnregisterHotKey(window_, kHotkeyId);
		hotkeyRegistered_ = false;
	}

	if (RegisterHotKey(window_, kHotkeyId,
		NativeHotkeyModifiers(hotkey), hotkey.virtualKey)) {
		registeredHotkey_ = hotkey;
		hotkeyRegistered_ = true;
		Logger::Write(LogLevel::Info, "Global capture hotkey registered");
		return true;
	}

	if (hadPrevious && RegisterHotKey(window_, kHotkeyId,
		NativeHotkeyModifiers(previous), previous.virtualKey)) {
		registeredHotkey_ = previous;
		hotkeyRegistered_ = true;
	}
	error = "快捷键 " + HotkeyText(hotkey) +
		" 注册失败，可能已被其他程序占用";
	Logger::Write(LogLevel::Warning, "Global hotkey registration failed");
	return false;
}

void App::UnregisterCaptureHotkey() noexcept {
	if (!hotkeyRegistered_ || !window_)
		return;
	UnregisterHotKey(window_, kHotkeyId);
	hotkeyRegistered_ = false;
}

bool App::ApplyOverlayHotkey(
	const CaptureHotkeySettings& hotkey, std::string& error) noexcept {
	if (IsOrdinaryUnmodifiedKey(hotkey)) {
		error = "覆盖层快捷键需要至少选择 Ctrl、Alt、Shift 或 Win";
		return false;
	}
	if (overlayHotkeyRegistered_ &&
		hotkey.control == registeredOverlayHotkey_.control &&
		hotkey.alt == registeredOverlayHotkey_.alt &&
		hotkey.shift == registeredOverlayHotkey_.shift &&
		hotkey.windows == registeredOverlayHotkey_.windows &&
		hotkey.virtualKey == registeredOverlayHotkey_.virtualKey)
		return true;

	const bool hadPrevious = overlayHotkeyRegistered_;
	const CaptureHotkeySettings previous = registeredOverlayHotkey_;
	if (hadPrevious) {
		UnregisterHotKey(window_, kOverlayHotkeyId);
		overlayHotkeyRegistered_ = false;
	}

	if (RegisterHotKey(window_, kOverlayHotkeyId,
		NativeHotkeyModifiers(hotkey), hotkey.virtualKey)) {
		registeredOverlayHotkey_ = hotkey;
		overlayHotkeyRegistered_ = true;
		Logger::Write(LogLevel::Info, "Overlay toggle hotkey registered");
		return true;
	}

	if (hadPrevious && RegisterHotKey(window_, kOverlayHotkeyId,
		NativeHotkeyModifiers(previous), previous.virtualKey)) {
		registeredOverlayHotkey_ = previous;
		overlayHotkeyRegistered_ = true;
	}
	error = "覆盖层快捷键 " + HotkeyText(hotkey) +
		" 注册失败，可能已被其他程序占用";
	Logger::Write(LogLevel::Warning, "Overlay toggle hotkey registration failed");
	return false;
}

void App::UnregisterOverlayHotkey() noexcept {
	if (!overlayHotkeyRegistered_ || !window_)
		return;
	UnregisterHotKey(window_, kOverlayHotkeyId);
	overlayHotkeyRegistered_ = false;
}

bool App::ApplyOverlayScrollHotkey(const CaptureHotkeySettings& hotkey,
	bool scrollDown, std::string& error) noexcept {
	if (IsOrdinaryUnmodifiedKey(hotkey)) {
		error = "覆盖层滚动快捷键需要至少选择 Ctrl、Alt、Shift 或 Win";
		return false;
	}
	const UINT id = scrollDown
		? kOverlayScrollDownHotkeyId : kOverlayScrollUpHotkeyId;
	bool& registered = scrollDown
		? overlayScrollDownHotkeyRegistered_ : overlayScrollUpHotkeyRegistered_;
	CaptureHotkeySettings& current = scrollDown
		? registeredOverlayScrollDownHotkey_ : registeredOverlayScrollUpHotkey_;
	if (registered &&
		hotkey.control == current.control && hotkey.alt == current.alt &&
		hotkey.shift == current.shift && hotkey.windows == current.windows &&
		hotkey.virtualKey == current.virtualKey)
		return true;

	const bool hadPrevious = registered;
	const CaptureHotkeySettings previous = current;
	if (hadPrevious) {
		UnregisterHotKey(window_, id);
		registered = false;
	}
	if (RegisterHotKey(window_, id,
		NativeHotkeyModifiers(hotkey), hotkey.virtualKey)) {
		current = hotkey;
		registered = true;
		Logger::Write(LogLevel::Info, scrollDown
			? "Overlay scroll-down hotkey registered"
			: "Overlay scroll-up hotkey registered");
		return true;
	}
	if (hadPrevious && RegisterHotKey(window_, id,
		NativeHotkeyModifiers(previous), previous.virtualKey)) {
		current = previous;
		registered = true;
	}
	error = std::string(scrollDown ? "覆盖层向下滚动快捷键 " :
		"覆盖层向上滚动快捷键 ") + HotkeyText(hotkey) +
		" 注册失败，可能已被其他程序占用";
	Logger::Write(LogLevel::Warning, scrollDown
		? "Overlay scroll-down hotkey registration failed"
		: "Overlay scroll-up hotkey registration failed");
	return false;
}

void App::UnregisterOverlayScrollHotkeys() noexcept {
	if (!window_)
		return;
	if (overlayScrollDownHotkeyRegistered_) {
		UnregisterHotKey(window_, kOverlayScrollDownHotkeyId);
		overlayScrollDownHotkeyRegistered_ = false;
	}
	if (overlayScrollUpHotkeyRegistered_) {
		UnregisterHotKey(window_, kOverlayScrollUpHotkeyId);
		overlayScrollUpHotkeyRegistered_ = false;
	}
}

void App::ScrollOverlayAnswer(int direction) noexcept {
	// A disconnected or hidden overlay simply ignores the command. In
	// particular, a tray-resident hub must not surface its window on failure.
	overlayControl_.SendScrollAnswer(direction);
}

void App::TriggerCapture() noexcept {
	aiClient_.Cancel();
	requestState_ = RequestState::Capturing;
	requestStartedTick_ = 0;
	lastError_.clear();
	answer_.clear();
	progressText_.clear();
	const bool restoreWindowAfterCapture =
		window_ && IsWindowVisible(window_) != FALSE;
	if (restoreWindowAfterCapture)
		HideMainWindow();
	DwmFlush();
	Sleep(75);
	DwmFlush();
	ScreenshotResult screenshot = CapturePrimaryDisplayPng();
	if (restoreWindowAfterCapture)
		ShowMainWindow();
	if (!screenshot) {
		lastError_ = screenshot.error;
		requestState_ = RequestState::Error;
		Logger::Write(LogLevel::Error, "Primary display capture failed");
		return;
	}
	screenshotWidth_ = screenshot.width;
	screenshotHeight_ = screenshot.height;
	if (IsBlank(settings_.apiBaseUrl) || IsBlank(settings_.apiKey) ||
		IsBlank(settings_.model)) {
		lastError_ = "截图成功，但无法发起问答：请在“设置”中填写 API Base URL、API Key 和 Model";
		requestState_ = RequestState::Error;
		Logger::Write(LogLevel::Warning,
			"Screenshot captured; AI request skipped because settings are incomplete");
		return;
	}
	activeRequestId_ = nextRequestId_++;
	lastSequence_ = 0;
	overlaySink_.SetRequestIdFilter(activeRequestId_);
	if (!aiClient_.Start(settings_, std::move(screenshot.png),
		activeRequestId_, answerTee_)) {
		lastError_ = "无法创建 AI 请求线程";
		requestState_ = RequestState::Error;
		return;
	}
	requestStartedTick_ = GetTickCount64();
	requestState_ = RequestState::Connecting;
	Logger::Write(LogLevel::Info, "Screenshot captured; request dispatched");
}

void App::DrainAnswerEvents() noexcept {
	for (auto& event : answerSink_.Drain()) {
		if (event.requestId != activeRequestId_ && event.type != AnswerEventType::Cleared)
			continue;
		if (event.sequence <= lastSequence_ && event.type != AnswerEventType::Cleared)
			continue;
		lastSequence_ = event.sequence;
		switch (event.type) {
		case AnswerEventType::Started:
			progressText_.clear();
			requestState_ = RequestState::Connecting;
			break;
		case AnswerEventType::Progress:
			progressText_ = std::move(event.payload);
			requestState_ = progressText_.empty()
				? RequestState::Waiting : RequestState::Thinking;
			break;
		case AnswerEventType::Delta:
			progressText_.clear();
			answer_ += event.payload;
			requestState_ = RequestState::Streaming;
			break;
		case AnswerEventType::Completed:
			progressText_.clear();
			requestState_ = RequestState::Completed;
			break;
		case AnswerEventType::Failed:
			progressText_.clear();
			lastError_ = event.payload;
			requestState_ = RequestState::Error;
			break;
		case AnswerEventType::Cancelled:
			progressText_.clear();
			requestState_ = RequestState::Cancelled;
			break;
		case AnswerEventType::Cleared:
			answer_.clear();
			progressText_.clear();
			requestStartedTick_ = 0;
			requestState_ = RequestState::Idle;
			break;
		}
	}
}

void App::ShowMainWindow() noexcept {
	if (!window_)
		return;
	ShowWindow(window_, SW_SHOW);
	if (IsIconic(window_))
		ShowWindow(window_, SW_RESTORE);
	SetForegroundWindow(window_);
}

void App::HideMainWindow() noexcept {
	if (window_)
		ShowWindow(window_, SW_HIDE);
}

void App::AddTrayIcon() noexcept {
	NOTIFYICONDATAW icon = { sizeof(icon) };
	icon.hWnd = window_;
	icon.uID = kTrayIconId;
	icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	icon.uCallbackMessage = kTrayMessage;
	icon.hIcon = static_cast<HICON>(LoadImageW(instance_,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
		LR_DEFAULTCOLOR));
	wcscpy_s(icon.szTip, L"CoolHelperHub");
	trayAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
}

void App::RemoveTrayIcon() noexcept {
	if (!trayAdded_)
		return;
	NOTIFYICONDATAW icon = { sizeof(icon) };
	icon.hWnd = window_;
	icon.uID = kTrayIconId;
	Shell_NotifyIconW(NIM_DELETE, &icon);
	trayAdded_ = false;
}

void App::ShowTrayMenu() noexcept {
	const HMENU menu = CreatePopupMenu();
	if (!menu)
		return;
	AppendMenuW(menu, MF_STRING, kTrayShow, L"显示窗口");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, kTrayExit, L"退出程序");
	POINT cursor = {};
	GetCursorPos(&cursor);
	SetForegroundWindow(window_);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
		cursor.x, cursor.y, 0, window_, nullptr);
	PostMessageW(window_, WM_NULL, 0, 0);
	DestroyMenu(menu);
}

void App::RequestExit() noexcept {
	running_ = false;
	PostQuitMessage(0);
}

const char* App::StateText() const noexcept {
	switch (requestState_) {
	case RequestState::Capturing: return "正在截图";
	case RequestState::Connecting: return "正在连接服务";
	case RequestState::Waiting: return "等待模型响应";
	case RequestState::Thinking: return "模型正在推理";
	case RequestState::Streaming: return "正在生成答案";
	case RequestState::Completed: return "已完成";
	case RequestState::Cancelled: return "已停止";
	case RequestState::Error: return "失败";
	default: return "就绪";
	}
}

LRESULT App::HandleMessage(
	HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
	if (imguiReady_ && ImGui_ImplWin32_WndProcHandler(
		window, message, wParam, lParam))
		return TRUE;

	switch (message) {
	case WM_SIZE:
		if (device_ && wParam != SIZE_MINIMIZED && swapChain_) {
			DestroyRenderTarget();
			swapChain_->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
				DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
		}
		return 0;
	case WM_DPICHANGED:
		if (const auto suggested = reinterpret_cast<const RECT*>(lParam))
			SetWindowPos(window, nullptr, suggested->left, suggested->top,
				suggested->right - suggested->left,
				suggested->bottom - suggested->top,
				SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xFFF0) == SC_KEYMENU)
			return 0;
		break;
	case WM_CLOSE:
		HideMainWindow();
		return 0;
	case WM_HOTKEY:
		if (wParam == kHotkeyId)
			TriggerCapture();
		else if (wParam == kOverlayHotkeyId)
			ToggleOverlayVisible();
		else if (wParam == kOverlayScrollDownHotkeyId)
			ScrollOverlayAnswer(1);
		else if (wParam == kOverlayScrollUpHotkeyId)
			ScrollOverlayAnswer(-1);
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case kTrayShow: ShowMainWindow(); break;
		case kTrayExit: RequestExit(); break;
		default: break;
		}
		return 0;
	case kTrayMessage:
		if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
			ShowTrayMenu();
		return 0;
	case kAnswerEventMessage:
		DrainAnswerEvents();
		return 0;
	case WM_DESTROY:
		running_ = false;
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK WindowProc(
	HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		app = static_cast<App*>(create->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(app));
	}
	if (app)
		return app->HandleMessage(window, message, wParam, lParam);
	return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace coolhelper
