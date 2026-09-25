#include "coolhelper/SettingsStore.h"

#include <Windows.h>

#include <filesystem>
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
	expected.systemPrompt = "系统提示";
	expected.userPrompt = "截图问题";
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
	bool passed = serialized.find(expected.apiKey) == std::string::npos;

	coolhelper::AppSettings actual;
	passed = passed && store.Load(actual, error) &&
		actual.apiBaseUrl == expected.apiBaseUrl &&
		actual.apiKey == expected.apiKey &&
		actual.model == expected.model &&
		actual.systemPrompt == expected.systemPrompt &&
		actual.userPrompt == expected.userPrompt &&
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
		migrated.systemPrompt.find("C++17") != std::string::npos &&
		migrated.userPrompt.find("C++11") != std::string::npos &&
		migrated.systemPrompt.find("C++20 实现") == std::string::npos &&
		migrated.overlayScrollDownHotkey.control &&
		migrated.overlayScrollDownHotkey.alt &&
		migrated.overlayScrollDownHotkey.virtualKey == VK_OEM_PLUS &&
		migrated.overlayScrollUpHotkey.control &&
		migrated.overlayScrollUpHotkey.alt &&
		migrated.overlayScrollUpHotkey.virtualKey == VK_OEM_MINUS;

	DeleteFileW(path.c_str());
	if (!passed)
		std::cerr << "DPAPI settings round-trip failed: " << error << '\n';
	return passed ? 0 : 1;
}
