#include "coolhelper/SettingsStore.h"

#include <Windows.h>

#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

int main() {
	wchar_t temporaryDirectory[MAX_PATH] = {};
	if (!GetTempPathW(MAX_PATH, temporaryDirectory))
		return 1;
	const auto path = std::filesystem::path(temporaryDirectory) /
		(L"CoolHelperHub.settings." + std::to_wstring(GetCurrentProcessId()) + L".json");

	coolhelper::AppSettings expected;
	expected.apiBaseUrl = "https://example.invalid/v1";
	expected.apiKey = "super-secret-test-key";
	expected.model = "vision-test-model";
	expected.requestTimeoutSeconds = 180;
	expected.systemPrompt = "系统提示";
	expected.userPrompt = "截图问题";
    expected.overlayDisplayMode = coolhelper::DisplayMode::AllDisplays;
	expected.captureHotkey.control = false;
	expected.captureHotkey.alt = true;
	expected.captureHotkey.shift = true;
	expected.captureHotkey.windows = false;
	expected.captureHotkey.virtualKey = VK_F8;
	expected.overlayScrollDownHotkey.shift = true;
	expected.overlayScrollDownHotkey.virtualKey = VK_ADD;
	expected.overlayScrollUpHotkey.windows = true;
	expected.overlayScrollUpHotkey.virtualKey = VK_SUBTRACT;

	coolhelper::SettingsStore store(path);
	std::string error;
	if (!store.Save(expected, error)) {
		std::cerr << "Save failed: " << error << '\n';
		return 1;
	}

	std::ifstream raw(path, std::ios::binary);
	const std::string serialized(
		(std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
	raw.close(); // Later migration cases atomically replace this test file.
	bool passed = serialized.find(expected.apiKey) == std::string::npos &&
		serialized.find("\"version\": 10") != std::string::npos;
	if (!passed)
		std::cerr << "settings serialization check failed\n";

	coolhelper::AppSettings actual;
	passed = passed && store.Load(actual, error) &&
		actual.apiBaseUrl == expected.apiBaseUrl &&
		actual.apiKey == expected.apiKey &&
		actual.model == expected.model &&
		actual.requestTimeoutSeconds == expected.requestTimeoutSeconds &&
		actual.systemPrompt == expected.systemPrompt &&
		actual.userPrompt == expected.userPrompt &&
        actual.overlayDisplayMode == expected.overlayDisplayMode &&
		actual.captureHotkey.control == expected.captureHotkey.control &&
		actual.captureHotkey.alt == expected.captureHotkey.alt &&
		actual.captureHotkey.shift == expected.captureHotkey.shift &&
		actual.captureHotkey.windows == expected.captureHotkey.windows &&
		actual.captureHotkey.virtualKey == expected.captureHotkey.virtualKey &&
		actual.overlayScrollDownHotkey.shift ==
			expected.overlayScrollDownHotkey.shift &&
		actual.overlayScrollDownHotkey.virtualKey ==
			expected.overlayScrollDownHotkey.virtualKey &&
		actual.overlayScrollUpHotkey.windows ==
			expected.overlayScrollUpHotkey.windows &&
		actual.overlayScrollUpHotkey.virtualKey ==
			expected.overlayScrollUpHotkey.virtualKey;
	if (!passed)
		std::cerr << "settings round-trip check failed\n";

	// Settings saved by earlier releases should receive the interview preset,
	// while custom prompts above remain untouched.
	{
		std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
		legacy << R"({
  "version": 2,
  "systemPrompt": "你是一个严谨的视觉问题分析助手。请直接给出清晰、准确的答案。",
  "userPrompt": "请分析截图中的内容并回答问题。"
})";
	}
	coolhelper::AppSettings migrated;
	passed = passed && store.Load(migrated, error) &&
		migrated.systemPrompt == coolhelper::kDefaultInterviewSystemPrompt &&
		migrated.userPrompt == coolhelper::kDefaultInterviewUserPrompt &&
        migrated.overlayDisplayMode == coolhelper::DisplayMode::Compatible &&
		migrated.systemPrompt.find("默认使用 C++11") != std::string::npos &&
		migrated.systemPrompt.find("高于 C++11") != std::string::npos &&
		migrated.systemPrompt.find("C++17") == std::string::npos &&
		migrated.userPrompt.find("C++11") != std::string::npos &&
		migrated.systemPrompt.find("C++20 实现") == std::string::npos &&
		migrated.overlayScrollDownHotkey.control &&
		migrated.overlayScrollDownHotkey.alt &&
		migrated.overlayScrollDownHotkey.virtualKey == VK_OEM_PLUS &&
		migrated.overlayScrollUpHotkey.control &&
		migrated.overlayScrollUpHotkey.alt &&
		migrated.overlayScrollUpHotkey.virtualKey == VK_OEM_MINUS;
	if (!passed)
		std::cerr << "legacy prompt migration check failed\n";

	// The previous algorithm preset used very long single lines. Migrate only
	// that exact preset to the paragraph-formatted version used by the UI.
	{
		std::ofstream legacyAlgorithm(path, std::ios::binary | std::ios::trunc);
		legacyAlgorithm << R"({
  "version": 7,
  "systemPrompt": "你是一名 C/C++ 算法面试助手。根据截图准确识别算法题，优先使用 C++11。回答保持简洁，只给出解题思路、关键点、完整代码、时间与空间复杂度、边界情况，以及可能追问和对应的简短答案提示。除非题目明确要求，否则不要使用更高版本的C++；信息不足时说明假设。",
  "userPrompt": "请解答截图中的算法面试题：先简述思路和关键点，再给出完整的 C++ 代码，最后说明时间复杂度、空间复杂度、边界情况，并列出面试官可能的追问及对应答案提示。"
})";
	}
	coolhelper::AppSettings migratedAlgorithm;
	passed = passed && store.Load(migratedAlgorithm, error) &&
		migratedAlgorithm.systemPrompt ==
			coolhelper::kAlgorithmInterviewSystemPrompt &&
		migratedAlgorithm.userPrompt ==
			coolhelper::kAlgorithmInterviewUserPrompt &&
		migratedAlgorithm.systemPrompt.find('\n') != std::string::npos &&
		migratedAlgorithm.userPrompt.find('\n') != std::string::npos;
	if (!passed)
		std::cerr << "legacy algorithm migration check failed\n";

	// Exercise shipped presets and the older C++20 generation rules, rather
	// than testing only the locally edited legacy algorithm string above.
	const auto replace = [](std::string& text, const char* from, const char* to) {
		const auto at = text.find(from);
		if (at != std::string::npos) text.replace(at, std::strlen(from), to);
	};
	const auto expectUpgrade = [&](const std::string& system, const std::string& user,
		const char* expectedSystem, const char* expectedUser) {
		coolhelper::AppSettings preset;
		preset.systemPrompt = system;
		preset.userPrompt = user;
		if (!store.Save(preset, error)) return false;
		coolhelper::AppSettings loaded;
		if (!store.Load(loaded, error)) return false;
		if (loaded.systemPrompt != expectedSystem || loaded.userPrompt != expectedUser) {
			std::cerr << "Preset upgrade mismatch\n";
			return false;
		}
		// Saving/loading an upgraded preset must not alter it a second time.
		if (!store.Save(loaded, error)) return false;
		coolhelper::AppSettings again;
		return store.Load(again, error) && again.systemPrompt == loaded.systemPrompt &&
			again.userPrompt == loaded.userPrompt;
	};
	std::string interview = coolhelper::kDefaultInterviewSystemPrompt;
	std::string interviewQuestion = coolhelper::kDefaultInterviewUserPrompt;
	replace(interview,
		"2. C/C++ 问题默认使用 C++11，关注对象生命周期、内存管理、未定义行为、并发和 ABI；除非题目明确要求，否则不要使用高于 C++11 的语法或库。",
		"2. C/C++ 问题默认以 C++17 为准并兼顾 C++11，关注对象生命周期、内存管理、未定义行为、并发和 ABI；除非题目明确要求，否则不要使用 C++20 语法或库。");
	replace(interview,
		"5. 算法或代码题默认给出 C++11 实现，只有题目明确要求时才使用更高版本，",
		"5. 算法或代码题优先给出兼容 C++17 的实现，可用 C++11 完成时一并说明，");
	replace(interviewQuestion,
		"若是代码题，请默认给出 C++11 实现，只有题目明确要求时才使用更高版本，说明",
		"若是代码题，请优先给出兼容 C++17 的实现，并尽量兼顾 C++11，说明");
	passed = expectUpgrade(interview, interviewQuestion,
		coolhelper::kDefaultInterviewSystemPrompt, coolhelper::kDefaultInterviewUserPrompt) && passed;
	replace(interview,
		"2. C/C++ 问题默认以 C++17 为准并兼顾 C++11，关注对象生命周期、内存管理、未定义行为、并发和 ABI；除非题目明确要求，否则不要使用 C++20 语法或库。",
		"2. C/C++ 问题需关注语言标准、对象生命周期、内存管理、未定义行为、并发和 ABI。");
	replace(interview,
		"5. 算法或代码题优先给出兼容 C++17 的实现，可用 C++11 完成时一并说明，并给出时间复杂度、空间复杂度和边界情况。",
		"5. 算法或代码题给出清晰的 C++20 实现，并说明时间复杂度、空间复杂度和边界情况。");
	replace(interviewQuestion,
		"若是代码题，请优先给出兼容 C++17 的实现，并尽量兼顾 C++11，说明复杂度和边界情况；",
		"若是代码题，请给出 C++20 实现、复杂度和边界情况；");
	passed = expectUpgrade(interview, interviewQuestion,
		coolhelper::kDefaultInterviewSystemPrompt, coolhelper::kDefaultInterviewUserPrompt) && passed;
	std::string algorithm = coolhelper::kAlgorithmInterviewSystemPrompt;
	std::string algorithmQuestion = coolhelper::kAlgorithmInterviewUserPrompt;
	replace(algorithm, "优先使用 C++11。", "优先使用 C++17 并兼容 C++11。");
	replace(algorithm, "不要使用高于 C++11 的语法或库", "不要使用 C++20");
	replace(algorithmQuestion,
		"2. 再给出完整的 C++11 代码，只有题目明确要求时才使用更高版本；",
		"2. 再给出完整的 C++ 代码；");
	passed = expectUpgrade(algorithm, algorithmQuestion,
		coolhelper::kAlgorithmInterviewSystemPrompt, coolhelper::kAlgorithmInterviewUserPrompt) && passed;
	replace(algorithm, "优先使用 C++17 并兼容 C++11。", "优先使用 C++11。");
	passed = expectUpgrade(algorithm, algorithmQuestion,
		coolhelper::kAlgorithmInterviewSystemPrompt, coolhelper::kAlgorithmInterviewUserPrompt) && passed;
	passed = expectUpgrade(
		"你是一名 C/C++ 算法面试助手。根据截图准确识别算法题，优先使用 C++17 并兼容 C++11。"
		"回答保持简洁，只给出解题思路、关键点、完整代码、时间与空间复杂度、边界情况，以及可能追问和对应的简短答案提示。"
		"除非题目明确要求，否则不要使用 C++20；信息不足时说明假设。",
		"请解答截图中的算法面试题：先简述思路和关键点，再给出完整的 C++ 代码，最后说明时间复杂度、空间复杂度、边界情况，并列出面试官可能的追问及对应答案提示。",
		coolhelper::kAlgorithmInterviewSystemPrompt, coolhelper::kAlgorithmInterviewUserPrompt) && passed;
	passed = expectUpgrade("自定义要求：解释 C++17 与 C++20 的差异。",
		"题目明确要求用 C++20 实现。", "自定义要求：解释 C++17 与 C++20 的差异。",
		"题目明确要求用 C++20 实现。") && passed;

    {
        std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
        invalid << R"({"overlayDisplayMode":999})";
    }
    coolhelper::AppSettings invalidMode;
    passed = passed && store.Load(invalidMode, error) &&
        invalidMode.overlayDisplayMode == coolhelper::DisplayMode::Compatible;
	{
		std::ofstream invalidTimeout(path, std::ios::binary | std::ios::trunc);
		invalidTimeout << R"({"requestTimeoutSeconds":1})";
	}
	coolhelper::AppSettings fallbackTimeout;
	passed = passed && store.Load(fallbackTimeout, error) &&
		fallbackTimeout.requestTimeoutSeconds ==
			coolhelper::kDefaultRequestTimeoutSeconds;
	{
		std::ofstream invalidTimeoutType(path, std::ios::binary | std::ios::trunc);
		invalidTimeoutType << R"({"requestTimeoutSeconds":"fast"})";
	}
	coolhelper::AppSettings fallbackTimeoutType;
	passed = passed && store.Load(fallbackTimeoutType, error) &&
		fallbackTimeoutType.requestTimeoutSeconds ==
			coolhelper::kDefaultRequestTimeoutSeconds;
	coolhelper::AppSettings invalidSave = expected;
	invalidSave.requestTimeoutSeconds =
		coolhelper::kMaxRequestTimeoutSeconds + 1;
	error.clear();
	passed = passed && !store.Save(invalidSave, error) && !error.empty();
	DeleteFileW(path.c_str());
	if (!passed)
		std::cerr << "DPAPI settings round-trip failed: " << error << '\n';
	return passed ? 0 : 1;
}
