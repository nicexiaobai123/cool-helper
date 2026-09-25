#include "coolhelper/SettingsStore.h"

#include "coolhelper/Logger.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Wincrypt.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace coolhelper {
namespace {

using nlohmann::json;

constexpr std::string_view kLegacySystemPrompt =
	"你是一个严谨的视觉问题分析助手。请直接给出清晰、准确的答案。";
constexpr std::string_view kLegacyUserPrompt = "请分析截图中的内容并回答问题。";
constexpr std::string_view kLegacyAlgorithmSystemPrompt =
	"你是一名 C/C++ 算法面试助手。根据截图准确识别算法题，优先使用 C++17 并兼容 C++11。"
	"回答保持简洁，只给出解题思路、关键点、完整代码、时间与空间复杂度、边界情况，"
	"以及可能追问和对应的简短答案提示。"
	"除非题目明确要求，否则不要使用 C++20；信息不足时说明假设。";
constexpr std::string_view kLegacyAlgorithmUserPrompt =
	"请解答截图中的算法面试题：先简述思路和关键点，再给出完整的 C++ 代码，"
	"最后说明时间复杂度、空间复杂度、边界情况，并列出面试官可能的追问及对应答案提示。";

void ReplaceOnce(
	std::string& text, std::string_view oldText, std::string_view newText) {
	const std::size_t position = text.find(oldText);
	if (position != std::string::npos)
		text.replace(position, oldText.size(), newText);
}

std::filesystem::path SettingsPath() {
	PWSTR rawPath = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath)))
		return std::filesystem::current_path() / L"settings.json";
	std::filesystem::path directory(rawPath);
	CoTaskMemFree(rawPath);
	directory /= L"CoolHelperHub";
	std::filesystem::create_directories(directory);
	return directory / L"settings.json";
}

std::string ToBase64(const BYTE* bytes, DWORD size) {
	DWORD required = 0;
	if (!CryptBinaryToStringA(bytes, size,
		CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &required))
		return {};
	std::string result(required, '\0');
	if (!CryptBinaryToStringA(bytes, size,
		CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &required))
		return {};
	if (!result.empty() && result.back() == '\0')
		result.pop_back();
	return result;
}

std::vector<BYTE> FromBase64(std::string_view text) {
	DWORD required = 0;
	if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()),
		CRYPT_STRING_BASE64, nullptr, &required, nullptr, nullptr))
		return {};
	std::vector<BYTE> result(required);
	if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()),
		CRYPT_STRING_BASE64, result.data(), &required, nullptr, nullptr))
		return {};
	result.resize(required);
	return result;
}

std::string Protect(std::string_view plaintext) {
	if (plaintext.empty())
		return {};
	DATA_BLOB input = {
		static_cast<DWORD>(plaintext.size()),
		reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))
	};
	DATA_BLOB output = {};
	if (!CryptProtectData(&input, L"CoolHelperHub API Key", nullptr, nullptr,
		nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
		return {};
	const std::string protectedText = ToBase64(output.pbData, output.cbData);
	LocalFree(output.pbData);
	return protectedText;
}

std::string Unprotect(std::string_view encoded) {
	if (encoded.empty())
		return {};
	auto encrypted = FromBase64(encoded);
	if (encrypted.empty())
		return {};
	DATA_BLOB input = {
		static_cast<DWORD>(encrypted.size()), encrypted.data()
	};
	DATA_BLOB output = {};
	if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
		CRYPTPROTECT_UI_FORBIDDEN, &output))
		return {};
	std::string plaintext(
		reinterpret_cast<const char*>(output.pbData), output.cbData);
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	return plaintext;
}

} // namespace

SettingsStore::SettingsStore()
	: path_(SettingsPath()) {}

SettingsStore::SettingsStore(std::filesystem::path path)
	: path_(std::move(path)) {}

bool SettingsStore::Load(AppSettings& settings, std::string& error) const noexcept {
	try {
		if (!std::filesystem::exists(path_))
			return true;
		std::ifstream stream(path_, std::ios::binary);
		if (!stream) {
			error = "无法打开设置文件";
			return false;
		}
		const json document = json::parse(stream);
		settings.apiBaseUrl = document.value("apiBaseUrl", settings.apiBaseUrl);
		settings.model = document.value("model", settings.model);
		settings.systemPrompt = document.value("systemPrompt", settings.systemPrompt);
		settings.userPrompt = document.value("userPrompt", settings.userPrompt);
		settings.overlayDllPath = document.value("overlayDllPath",
			settings.overlayDllPath);
		// Upgrade only the original defaults. User-authored prompts are preserved.
		if (settings.systemPrompt == kLegacySystemPrompt)
			settings.systemPrompt = kDefaultInterviewSystemPrompt;
		if (settings.userPrompt == kLegacyUserPrompt)
			settings.userPrompt = kDefaultInterviewUserPrompt;
		if (settings.systemPrompt == kLegacyAlgorithmSystemPrompt)
			settings.systemPrompt = kAlgorithmInterviewSystemPrompt;
		if (settings.userPrompt == kLegacyAlgorithmUserPrompt)
			settings.userPrompt = kAlgorithmInterviewUserPrompt;
		// Upgrade the generated v3 interview preset without touching unrelated
		// custom text. These exact phrases only appeared in our previous preset.
		ReplaceOnce(settings.systemPrompt,
			"2. C/C++ 问题需关注语言标准、对象生命周期、内存管理、未定义行为、并发和 ABI。",
			"2. C/C++ 问题默认以 C++17 为准并兼顾 C++11，关注对象生命周期、内存管理、"
			"未定义行为、并发和 ABI；除非题目明确要求，否则不要使用 C++20 语法或库。");
		ReplaceOnce(settings.systemPrompt,
			"5. 算法或代码题给出清晰的 C++20 实现，并说明时间复杂度、空间复杂度和边界情况。",
			"5. 算法或代码题优先给出兼容 C++17 的实现，可用 C++11 完成时一并说明，"
			"并给出时间复杂度、空间复杂度和边界情况。");
		ReplaceOnce(settings.userPrompt,
			"若是代码题，请给出 C++20 实现、复杂度和边界情况；",
			"若是代码题，请优先给出兼容 C++17 的实现，并尽量兼顾 C++11，"
			"说明复杂度和边界情况；");
		ReplaceOnce(settings.systemPrompt,
			"6. 指出面试官可能继续追问的点以及容易答错的地方。",
			"6. 指出面试官可能继续追问的点以及容易答错的地方，"
			"并为每个追问给出简短答案提示。");
		ReplaceOnce(settings.userPrompt,
			"再列出关键原理、易错点和可能的追问。",
			"再列出关键原理、易错点和可能的追问，并为每个追问给出简短答案提示。");
		ReplaceOnce(settings.systemPrompt,
			"回答保持简洁，只给出解题思路、关键点、完整代码、时间与空间复杂度、边界情况和可能追问。",
			"回答保持简洁，只给出解题思路、关键点、完整代码、时间与空间复杂度、边界情况，"
			"以及可能追问和对应的简短答案提示。");
		ReplaceOnce(settings.userPrompt,
			"最后说明时间复杂度、空间复杂度、边界情况和面试官可能的追问。",
			"最后说明时间复杂度、空间复杂度、边界情况，"
			"并列出面试官可能的追问及对应答案提示。");
		if (settings.systemPrompt.find("不要输出 LaTeX") == std::string::npos) {
			ReplaceOnce(settings.systemPrompt,
				"8. 使用中文回答，专业术语、API、代码和标识符保留英文；内容准确、紧凑，不说无关内容。",
				"8. 使用中文回答，专业术语、API、代码和标识符保留英文；内容准确、紧凑，不说无关内容。\n"
				"9. 复杂度和公式使用普通文本或 Unicode 符号，例如 O(K × L)，"
				"不要输出 LaTeX 的 $...$、\\cdot 等标记。");
			ReplaceOnce(settings.systemPrompt,
				"4. 可能的追问和对应的简短答案提示。\n除非题目明确要求",
				"4. 可能的追问和对应的简短答案提示；\n"
				"5. 复杂度和公式使用普通文本或 Unicode 符号，"
				"不要输出 LaTeX 标记。\n除非题目明确要求");
		}
		if (settings.userPrompt.find("不要使用 LaTeX") == std::string::npos) {
			ReplaceOnce(settings.userPrompt,
				"复杂度和边界情况；若题目信息不完整，请说明你的假设。不要重复题目，不要输出无关内容。",
				"复杂度和边界情况；若题目信息不完整，请说明你的假设。"
				"复杂度和公式不要使用 LaTeX 标记。不要重复题目，不要输出无关内容。");
			ReplaceOnce(settings.userPrompt,
				"4. 列出面试官可能的追问及对应答案提示。",
				"4. 列出面试官可能的追问及对应答案提示；\n"
				"5. 复杂度和公式不要使用 LaTeX 标记。");
		}
		if (const auto hotkey = document.find("captureHotkey");
			hotkey != document.end() && hotkey->is_object()) {
			settings.captureHotkey.control = hotkey->value(
				"control", settings.captureHotkey.control);
			settings.captureHotkey.alt = hotkey->value(
				"alt", settings.captureHotkey.alt);
			settings.captureHotkey.shift = hotkey->value(
				"shift", settings.captureHotkey.shift);
			settings.captureHotkey.windows = hotkey->value(
				"windows", settings.captureHotkey.windows);
			const auto virtualKey = hotkey->value(
				"virtualKey", settings.captureHotkey.virtualKey);
			if (virtualKey > 0 && virtualKey <= 0xFF)
				settings.captureHotkey.virtualKey = virtualKey;
		}
		if (const auto hotkey = document.find("overlayToggleHotkey");
			hotkey != document.end() && hotkey->is_object()) {
			settings.overlayToggleHotkey.control = hotkey->value(
				"control", settings.overlayToggleHotkey.control);
			settings.overlayToggleHotkey.alt = hotkey->value(
				"alt", settings.overlayToggleHotkey.alt);
			settings.overlayToggleHotkey.shift = hotkey->value(
				"shift", settings.overlayToggleHotkey.shift);
			settings.overlayToggleHotkey.windows = hotkey->value(
				"windows", settings.overlayToggleHotkey.windows);
			const auto virtualKey = hotkey->value(
				"virtualKey", settings.overlayToggleHotkey.virtualKey);
			if (virtualKey > 0 && virtualKey <= 0xFF)
				settings.overlayToggleHotkey.virtualKey = virtualKey;
		}
		if (const auto hotkey = document.find("overlayScrollDownHotkey");
			hotkey != document.end() && hotkey->is_object()) {
			settings.overlayScrollDownHotkey.control = hotkey->value(
				"control", settings.overlayScrollDownHotkey.control);
			settings.overlayScrollDownHotkey.alt = hotkey->value(
				"alt", settings.overlayScrollDownHotkey.alt);
			settings.overlayScrollDownHotkey.shift = hotkey->value(
				"shift", settings.overlayScrollDownHotkey.shift);
			settings.overlayScrollDownHotkey.windows = hotkey->value(
				"windows", settings.overlayScrollDownHotkey.windows);
			const auto virtualKey = hotkey->value(
				"virtualKey", settings.overlayScrollDownHotkey.virtualKey);
			if (virtualKey > 0 && virtualKey <= 0xFF)
				settings.overlayScrollDownHotkey.virtualKey = virtualKey;
		}
		if (const auto hotkey = document.find("overlayScrollUpHotkey");
			hotkey != document.end() && hotkey->is_object()) {
			settings.overlayScrollUpHotkey.control = hotkey->value(
				"control", settings.overlayScrollUpHotkey.control);
			settings.overlayScrollUpHotkey.alt = hotkey->value(
				"alt", settings.overlayScrollUpHotkey.alt);
			settings.overlayScrollUpHotkey.shift = hotkey->value(
				"shift", settings.overlayScrollUpHotkey.shift);
			settings.overlayScrollUpHotkey.windows = hotkey->value(
				"windows", settings.overlayScrollUpHotkey.windows);
			const auto virtualKey = hotkey->value(
				"virtualKey", settings.overlayScrollUpHotkey.virtualKey);
			if (virtualKey > 0 && virtualKey <= 0xFF)
				settings.overlayScrollUpHotkey.virtualKey = virtualKey;
		}
		const std::string protectedKey = document.value("apiKeyProtected", "");
		settings.apiKey = Unprotect(protectedKey);
		if (!protectedKey.empty() && settings.apiKey.empty()) {
			error = "API Key 无法通过当前 Windows 用户解密";
			return false;
		}
		return true;
	}
	catch (const std::exception& exception) {
		error = std::string("设置文件解析失败: ") + exception.what();
		Logger::Write(LogLevel::Error, "Settings load failed");
		return false;
	}
}

bool SettingsStore::Save(
	const AppSettings& settings,
	std::string& error) const noexcept {
	try {
		const std::string protectedKey = Protect(settings.apiKey);
		if (!settings.apiKey.empty() && protectedKey.empty()) {
			error = "API Key 加密失败";
			return false;
		}
		const json document = {
			{ "version", 8 },
			{ "apiBaseUrl", settings.apiBaseUrl },
			{ "apiKeyProtected", protectedKey },
			{ "model", settings.model },
			{ "systemPrompt", settings.systemPrompt },
			{ "userPrompt", settings.userPrompt },
			{ "overlayDllPath", settings.overlayDllPath },
			{ "captureHotkey", {
				{ "control", settings.captureHotkey.control },
				{ "alt", settings.captureHotkey.alt },
				{ "shift", settings.captureHotkey.shift },
				{ "windows", settings.captureHotkey.windows },
				{ "virtualKey", settings.captureHotkey.virtualKey }
			} },
			{ "overlayToggleHotkey", {
				{ "control", settings.overlayToggleHotkey.control },
				{ "alt", settings.overlayToggleHotkey.alt },
				{ "shift", settings.overlayToggleHotkey.shift },
				{ "windows", settings.overlayToggleHotkey.windows },
				{ "virtualKey", settings.overlayToggleHotkey.virtualKey }
			} },
			{ "overlayScrollDownHotkey", {
				{ "control", settings.overlayScrollDownHotkey.control },
				{ "alt", settings.overlayScrollDownHotkey.alt },
				{ "shift", settings.overlayScrollDownHotkey.shift },
				{ "windows", settings.overlayScrollDownHotkey.windows },
				{ "virtualKey", settings.overlayScrollDownHotkey.virtualKey }
			} },
			{ "overlayScrollUpHotkey", {
				{ "control", settings.overlayScrollUpHotkey.control },
				{ "alt", settings.overlayScrollUpHotkey.alt },
				{ "shift", settings.overlayScrollUpHotkey.shift },
				{ "windows", settings.overlayScrollUpHotkey.windows },
				{ "virtualKey", settings.overlayScrollUpHotkey.virtualKey }
			} }
		};
		const auto temporary = path_.wstring() + L".tmp";
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			if (!stream) {
				error = "无法写入临时设置文件";
				return false;
			}
			stream << document.dump(2);
			if (!stream) {
				error = "设置文件写入失败";
				return false;
			}
		}
		if (!MoveFileExW(temporary.c_str(), path_.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			error = "无法替换设置文件";
			DeleteFileW(temporary.c_str());
			return false;
		}
		return true;
	}
	catch (const std::exception& exception) {
		error = std::string("保存设置失败: ") + exception.what();
		Logger::Write(LogLevel::Error, "Settings save failed");
		return false;
	}
}

} // namespace coolhelper
