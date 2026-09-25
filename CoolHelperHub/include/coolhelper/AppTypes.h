#pragma once

#include <cstdint>
#include <string>

namespace coolhelper {

inline constexpr char kDefaultInterviewSystemPrompt[] =
	"你是一名资深的 C/C++、Windows 客户端和逆向工程面试助手。"
	"请根据截图准确识别面试题，并生成适合候选人在面试现场直接表达的答案。\n\n"
	"回答要求：\n"
	"1. 先给出结论或一段可直接口述的简洁答案，再解释关键原理。\n"
	"2. C/C++ 问题默认以 C++17 为准并兼顾 C++11，关注对象生命周期、内存管理、"
	"未定义行为、并发和 ABI；除非题目明确要求，否则不要使用 C++20 语法或库。\n"
	"3. 客户端问题需关注 Win32、线程模型、IPC、图形渲染、性能、稳定性和工程实践。\n"
	"4. 逆向问题需关注 x86/x64 汇编、调用约定、PE、调试、Hook、CFG 和常见保护机制，"
	"仅讨论合法授权的分析场景。\n"
	"5. 算法或代码题优先给出兼容 C++17 的实现，可用 C++11 完成时一并说明，"
	"并给出时间复杂度、空间复杂度和边界情况。\n"
	"6. 指出面试官可能继续追问的点以及容易答错的地方，并为每个追问给出简短答案提示。\n"
	"7. 截图信息不完整时明确说明假设并分别回答，不要编造题目细节。\n"
	"8. 使用中文回答，专业术语、API、代码和标识符保留英文；内容准确、紧凑，不说无关内容。\n"
	"9. 复杂度和公式使用普通文本或 Unicode 符号，例如 O(K × L)，不要输出 LaTeX 的 $...$、\\cdot 等标记。";

inline constexpr char kDefaultInterviewUserPrompt[] =
	"请读取截图中的面试题并作答。先给出适合面试现场直接口述的答案，"
	"再列出关键原理、易错点和可能的追问，并为每个追问给出简短答案提示。"
	"若是代码题，请优先给出兼容 C++17 的实现，"
	"并尽量兼顾 C++11，说明"
	"复杂度和边界情况；若题目信息不完整，请说明你的假设。"
	"复杂度和公式不要使用 LaTeX 标记。不要重复题目，不要输出无关内容。";

inline constexpr char kAlgorithmInterviewSystemPrompt[] =
	"你是一名 C/C++ 算法面试助手。\n"
	"根据截图准确识别算法题，优先使用 C++17 并兼容 C++11。\n"
	"回答保持简洁，依次给出：\n"
	"1. 解题思路和关键点；\n"
	"2. 完整代码；\n"
	"3. 时间与空间复杂度、边界情况；\n"
	"4. 可能的追问和对应的简短答案提示；\n"
	"5. 复杂度和公式使用普通文本或 Unicode 符号，不要输出 LaTeX 标记。\n"
	"除非题目明确要求，否则不要使用 C++20；信息不足时说明假设。";

inline constexpr char kAlgorithmInterviewUserPrompt[] =
	"请解答截图中的算法面试题：\n"
	"1. 先简述思路和关键点；\n"
	"2. 再给出完整的 C++ 代码；\n"
	"3. 最后说明时间复杂度、空间复杂度和边界情况；\n"
	"4. 列出面试官可能的追问及对应答案提示；\n"
	"5. 复杂度和公式不要使用 LaTeX 标记。";

enum class RequestState {
	Idle,
	Capturing,
	Streaming,
	Completed,
	Cancelled,
	Error
};

struct CaptureHotkeySettings {
	bool control = true;
	bool alt = true;
	bool shift = false;
	bool windows = false;
	std::uint32_t virtualKey = 'S';
};

struct AppSettings {
	std::string apiBaseUrl = "https://api.openai.com/v1";
	std::string apiKey;
	std::string model = "gpt-4o-mini";
	std::string systemPrompt = kDefaultInterviewSystemPrompt;
	std::string userPrompt = kDefaultInterviewUserPrompt;
	CaptureHotkeySettings captureHotkey;
	CaptureHotkeySettings overlayToggleHotkey{
		.control = true, .alt = true, .virtualKey = 'H'};
	CaptureHotkeySettings overlayScrollDownHotkey{
		.control = true, .alt = true, .virtualKey = 0xBBu}; // VK_OEM_PLUS
	CaptureHotkeySettings overlayScrollUpHotkey{
		.control = true, .alt = true, .virtualKey = 0xBDu}; // VK_OEM_MINUS
	// Empty means "<exe directory>\DwmCfgOverlayLab.dll".
	std::string overlayDllPath;
};

enum class AnswerEventType {
	Started,
	Delta,
	Completed,
	Failed,
	Cancelled,
	Cleared
};

struct AnswerEvent {
	std::uint64_t requestId = 0;
	std::uint64_t sequence = 0;
	AnswerEventType type = AnswerEventType::Started;
	std::string payload;
};

} // namespace coolhelper
