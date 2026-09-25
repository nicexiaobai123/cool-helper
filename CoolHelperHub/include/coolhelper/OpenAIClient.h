#pragma once

#include <Windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AnswerSink.h"
#include "AppTypes.h"

namespace coolhelper {

class OpenAIClient final {
public:
	OpenAIClient() = default;
	~OpenAIClient();

	OpenAIClient(const OpenAIClient&) = delete;
	OpenAIClient& operator=(const OpenAIClient&) = delete;

	bool Start(
		const AppSettings& settings,
		std::vector<std::uint8_t> png,
		std::uint64_t requestId,
		IAnswerSink& sink) noexcept;
	void Cancel() noexcept;
	bool IsRunning() const noexcept { return running_.load(); }

	static std::wstring BuildChatCompletionsUrl(std::string_view baseUrl);

private:
	void Run(
		AppSettings settings,
		std::vector<std::uint8_t> png,
		std::uint64_t requestId,
		IAnswerSink* sink) noexcept;
	void SetActiveRequest(HINTERNET request) noexcept;
	void ReleaseActiveRequest(HINTERNET request) noexcept;

	std::thread worker_;
	std::atomic_bool cancelRequested_ = false;
	std::atomic_bool running_ = false;
	std::mutex requestMutex_;
	HINTERNET activeRequest_ = nullptr;
};

} // namespace coolhelper

