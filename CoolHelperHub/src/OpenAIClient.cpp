#include "coolhelper/OpenAIClient.h"

#include "coolhelper/Logger.h"
#include "coolhelper/Screenshot.h"
#include "coolhelper/SseParser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace coolhelper {
namespace {

using nlohmann::json;

constexpr char kCancelledMessage[] = "请求已取消";

class InternetHandle final {
public:
	explicit InternetHandle(HINTERNET value = nullptr) noexcept : value_(value) {}
	~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
	InternetHandle(const InternetHandle&) = delete;
	InternetHandle& operator=(const InternetHandle&) = delete;
	operator HINTERNET() const noexcept { return value_; }
	HINTERNET Get() const noexcept { return value_; }
private:
	HINTERNET value_ = nullptr;
};

class TimeoutWatchdog final {
public:
	TimeoutWatchdog(ULONGLONG timeoutAt, std::function<void()> onTimeout)
		: onTimeout_(std::move(onTimeout)) {
		worker_ = std::thread([this, timeoutAt] {
			std::unique_lock lock(mutex_);
			const ULONGLONG now = GetTickCount64();
			if (timeoutAt > now) {
				condition_.wait_for(lock,
					std::chrono::milliseconds(timeoutAt - now),
					[this] { return finished_; });
			}
			if (finished_)
				return;
			lock.unlock();
			onTimeout_();
		});
	}

	~TimeoutWatchdog() { Stop(); }

	TimeoutWatchdog(const TimeoutWatchdog&) = delete;
	TimeoutWatchdog& operator=(const TimeoutWatchdog&) = delete;

	void Stop() noexcept {
		{
			std::scoped_lock lock(mutex_);
			if (finished_)
				return;
			finished_ = true;
		}
		condition_.notify_all();
		if (worker_.joinable())
			worker_.join();
	}

private:
	std::function<void()> onTimeout_;
	std::mutex mutex_;
	std::condition_variable condition_;
	bool finished_ = false;
	std::thread worker_;
};

std::wstring Utf8ToWide(std::string_view text) {
	if (text.empty())
		return {};
	const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (required <= 0)
		throw std::runtime_error("字符串不是有效 UTF-8");
	std::wstring result(static_cast<std::size_t>(required), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
		static_cast<int>(text.size()), result.data(), required);
	return result;
}

std::string DescribeWindowsNetworkError(DWORD error, const char* action) {
	const std::string suffix = "（Windows 错误码 " + std::to_string(error) + "）";
	switch (error) {
	case ERROR_WINHTTP_TIMEOUT:
	case ERROR_TIMEOUT:
		return std::string(action) + "超时" + suffix;
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
		return "无法解析 API 域名" + suffix + "，请检查网络、DNS 或 API Base URL";
	case ERROR_WINHTTP_CANNOT_CONNECT:
		return "无法连接 API 服务" + suffix + "，请检查网络、代理、防火墙或 API Base URL";
	case ERROR_WINHTTP_CONNECTION_ERROR:
	case ERROR_NETWORK_UNREACHABLE:
	case ERROR_HOST_UNREACHABLE:
		return "网络连接中断" + suffix + "，请稍后重试";
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
	case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
	case ERROR_WINHTTP_SECURE_INVALID_CA:
	case ERROR_WINHTTP_SECURE_CERT_REVOKED:
		return "HTTPS 连接校验失败" + suffix + "，请检查系统时间、代理或证书配置";
	case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
		return "服务返回了无法识别的 HTTP 响应" + suffix;
	case ERROR_WINHTTP_INVALID_URL:
	case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
		return "API Base URL 无效" + suffix;
	case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
	case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT:
		return "系统代理配置失败" + suffix + "，请检查 Windows 网络代理设置";
	default:
		return std::string(action) + "失败" + suffix;
	}
}

std::string WindowsError(const char* action) {
	return DescribeWindowsNetworkError(GetLastError(), action);
}

std::string RequestTimeoutMessage(std::uint32_t timeoutSeconds) {
	std::ostringstream message;
	message << "请求超时：" << timeoutSeconds
		<< " 秒内未完成。可检查网络或在设置中增加超时时间";
	return message.str();
}

std::string ExtractErrorMessage(const std::string& body) {
	try {
		const json document = json::parse(body);
		if (document.is_string())
			return document.get<std::string>();

		auto extractObject = [](const json& value) -> std::string {
			if (!value.is_object())
				return {};
			std::string message;
			if (value.contains("message") && value["message"].is_string())
				message = value["message"].get<std::string>();
			else if (value.contains("data") && value["data"].is_string())
				message = value["data"].get<std::string>();
			if (!message.empty() && value.contains("code")) {
				const auto& code = value["code"];
				if (code.is_number_integer())
					message += " (code " + std::to_string(code.get<long long>()) + ')';
				else if (code.is_string())
					message += " (code " + code.get<std::string>() + ')';
			}
			return message;
		};

		if (document.contains("error")) {
			const auto& error = document["error"];
			if (error.is_string())
				return error.get<std::string>();
			if (std::string message = extractObject(error); !message.empty())
				return message;
		}
		// SiliconFlow and some other OpenAI-compatible providers return
		// { "code": ..., "message": ..., "data": ... } at the top level.
		if (std::string message = extractObject(document); !message.empty())
			return message;
	}
	catch (...) {
	}
	return {};
}

std::string ExtractContent(const json& value) {
	if (value.is_string())
		return value.get<std::string>();
	if (value.is_object() && value.contains("content"))
		return ExtractContent(value["content"]);
	if (!value.is_array())
		return {};
	std::string result;
	for (const auto& item : value) {
		if (item.is_object() && item.contains("text") && item["text"].is_string())
			result += item["text"].get<std::string>();
	}
	return result;
}

std::string ExtractReasoningContent(const json& delta) {
	static constexpr const char* keys[] = {
		"reasoning_content", "reasoning", "thinking"
	};
	for (const char* key : keys) {
		if (!delta.contains(key) || delta[key].is_null())
			continue;
		if (std::string content = ExtractContent(delta[key]); !content.empty())
			return content;
	}
	return {};
}

void AppendReasoningPreview(std::string& preview, std::string_view delta) {
	bool pendingSpace = !preview.empty() && preview.back() == ' ';
	for (const unsigned char value : delta) {
		if (value == '\r' || value == '\n' || value == '\t' || value == ' ') {
			pendingSpace = !preview.empty();
			continue;
		}
		if (pendingSpace && !preview.empty() && preview.back() != ' ')
			preview.push_back(' ');
		pendingSpace = false;
		preview.push_back(static_cast<char>(value));
	}

	// This is only an activity hint. Keep a short tail so it remains on one
	// overlay line: at most 108 UTF-8 bytes and 36 visible code points.
	constexpr std::size_t kPreviewByteLimit = 108;
	constexpr std::size_t kPreviewCodepointLimit = 36;
	std::size_t byteStart = preview.size() > kPreviewByteLimit
		? preview.size() - kPreviewByteLimit : 0;
	while (byteStart < preview.size() &&
		(static_cast<unsigned char>(preview[byteStart]) & 0xC0u) == 0x80u)
		++byteStart;

	std::size_t codepointStart = preview.size();
	std::size_t codepointCount = 0;
	while (codepointStart > 0 && codepointCount < kPreviewCodepointLimit) {
		--codepointStart;
		while (codepointStart > 0 &&
			(static_cast<unsigned char>(preview[codepointStart]) & 0xC0u) == 0x80u)
			--codepointStart;
		++codepointCount;
	}

	const std::size_t start = byteStart > codepointStart
		? byteStart : codepointStart;
	if (start == 0)
		return;
	preview.erase(0, start);
	preview.insert(0, "…");
}

} // namespace

OpenAIClient::~OpenAIClient() {
	Cancel();
}

std::wstring OpenAIClient::BuildChatCompletionsUrl(std::string_view baseUrl) {
	std::string normalized(baseUrl);
	while (!normalized.empty() && normalized.back() == '/')
		normalized.pop_back();
	std::string lower = normalized;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	if (lower.find("/api/anthropic") != std::string::npos) {
		throw std::runtime_error(
			"当前客户端使用 OpenAI Chat Completions 协议，不能使用 Anthropic API 地址；"
			"请填写服务商的 OpenAI-compatible Base URL");
	}
	if (!lower.ends_with("/chat/completions"))
		normalized += "/chat/completions";
	return Utf8ToWide(normalized);
}

bool OpenAIClient::Start(
	const AppSettings& settings,
	std::vector<std::uint8_t> png,
	std::uint64_t requestId,
	IAnswerSink& sink) noexcept {
	Cancel();
	cancelRequested_.store(false);
	timeoutRequested_.store(false);
	running_.store(true);
	try {
		worker_ = std::thread(&OpenAIClient::Run, this,
			settings, std::move(png), requestId, &sink);
		return true;
	}
	catch (...) {
		running_.store(false);
		return false;
	}
}

void OpenAIClient::Cancel() noexcept {
	cancelRequested_.store(true);
	InterruptActiveRequest();
	if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
		worker_.join();
	running_.store(false);
}

void OpenAIClient::InterruptActiveRequest() noexcept {
	HINTERNET request = nullptr;
	{
		std::scoped_lock lock(requestMutex_);
		request = activeRequest_;
		activeRequest_ = nullptr;
	}
	if (request)
		WinHttpCloseHandle(request);
}

void OpenAIClient::SetActiveRequest(HINTERNET request) noexcept {
	std::scoped_lock lock(requestMutex_);
	activeRequest_ = request;
}

void OpenAIClient::ReleaseActiveRequest(HINTERNET request) noexcept {
	bool close = false;
	{
		std::scoped_lock lock(requestMutex_);
		if (activeRequest_ == request) {
			activeRequest_ = nullptr;
			close = true;
		}
	}
	if (close)
		WinHttpCloseHandle(request);
}

void OpenAIClient::Run(
	AppSettings settings,
	std::vector<std::uint8_t> png,
	std::uint64_t requestId,
	IAnswerSink* sink) noexcept {
	std::uint64_t sequence = 0;
	auto publish = [&](AnswerEventType type, std::string payload = {}) {
		if (sink)
			sink->Publish({ requestId, ++sequence, type, std::move(payload) });
	};
	auto finish = [&] { running_.store(false); };
	const ULONGLONG timeoutMs =
		static_cast<ULONGLONG>(settings.requestTimeoutSeconds) * 1000;
	const ULONGLONG timeoutAt = timeoutMs == 0 ? 0 : GetTickCount64() + timeoutMs;
	auto throwIfTimedOut = [&] {
		if (cancelRequested_.load())
			throw std::runtime_error(kCancelledMessage);
		if (timeoutRequested_.load() ||
			(timeoutAt && GetTickCount64() >= timeoutAt)) {
			timeoutRequested_.store(true);
			InterruptActiveRequest();
			throw std::runtime_error(
				RequestTimeoutMessage(settings.requestTimeoutSeconds));
		}
	};

	try {
		std::unique_ptr<TimeoutWatchdog> timeoutWatchdog;
		if (timeoutAt) {
			timeoutWatchdog = std::make_unique<TimeoutWatchdog>(timeoutAt, [this] {
				timeoutRequested_.store(true);
				InterruptActiveRequest();
			});
		}
		publish(AnswerEventType::Started);
		Logger::Write(LogLevel::Info, "AI request started");
		throwIfTimedOut();
		const std::string imageBase64 = Base64Encode(png);
		throwIfTimedOut();
		png.clear();
		png.shrink_to_fit();
		if (imageBase64.empty())
			throw std::runtime_error("截图 Base64 编码失败");
		throwIfTimedOut();

		const json requestDocument = {
			{ "model", settings.model },
			{ "stream", true },
			{ "messages", json::array({
				{
					{ "role", "system" },
					{ "content", settings.systemPrompt }
				},
				{
					{ "role", "user" },
					{ "content", json::array({
						{
							{ "type", "text" },
							{ "text", settings.userPrompt }
						},
						{
							{ "type", "image_url" },
							{ "image_url", {
								{ "url", "data:image/png;base64," + imageBase64 }
							} }
						}
					}) }
				}
			}) }
		};
		const std::string body = requestDocument.dump();
		if (body.size() > MAXDWORD)
			throw std::runtime_error("请求内容过大");
		throwIfTimedOut();

		const std::wstring url = BuildChatCompletionsUrl(settings.apiBaseUrl);
		URL_COMPONENTS components = { sizeof(components) };
		components.dwSchemeLength = static_cast<DWORD>(-1);
		components.dwHostNameLength = static_cast<DWORD>(-1);
		components.dwUrlPathLength = static_cast<DWORD>(-1);
		components.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components))
			throw std::runtime_error("API Base URL 无效");
		if (components.nScheme != INTERNET_SCHEME_HTTP &&
			components.nScheme != INTERNET_SCHEME_HTTPS)
			throw std::runtime_error("API URL 仅支持 HTTP 或 HTTPS");

		const std::wstring host(components.lpszHostName, components.dwHostNameLength);
		std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
		if (components.dwExtraInfoLength)
			path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

		throwIfTimedOut();
		publish(AnswerEventType::Progress, "正在建立网络连接");
		InternetHandle session(WinHttpOpen(L"CoolHelperHub/0.1",
			WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0));
		if (!session)
			throw std::runtime_error(WindowsError("WinHttpOpen"));
		DWORD operationTimeoutMs = 120000;
		if (timeoutAt) {
			const ULONGLONG now = GetTickCount64();
			const ULONGLONG remainingMs = timeoutAt > now ? timeoutAt - now : 1;
			operationTimeoutMs = remainingMs >= MAXDWORD
				? MAXDWORD : static_cast<DWORD>(remainingMs);
		}
		WinHttpSetTimeouts(session, operationTimeoutMs, operationTimeoutMs,
			operationTimeoutMs, operationTimeoutMs);
		InternetHandle connection(WinHttpConnect(
			session, host.c_str(), components.nPort, 0));
		if (!connection)
			throw std::runtime_error(WindowsError("WinHttpConnect"));

		throwIfTimedOut();
		publish(AnswerEventType::Progress, "正在发送截图请求");
		const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
			? WINHTTP_FLAG_SECURE : 0;
		HINTERNET request = WinHttpOpenRequest(connection, L"POST", path.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		if (!request)
			throw std::runtime_error(WindowsError("WinHttpOpenRequest"));
		SetActiveRequest(request);
		throwIfTimedOut();

		const std::wstring headers =
			L"Content-Type: application/json\r\nAccept: text/event-stream\r\n"
			L"Authorization: Bearer " + Utf8ToWide(settings.apiKey) + L"\r\n";
		if (cancelRequested_.load())
			throw std::runtime_error(kCancelledMessage);
		if (!WinHttpSendRequest(request, headers.c_str(),
			static_cast<DWORD>(headers.size()),
			const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
			static_cast<DWORD>(body.size()), 0) ||
			!WinHttpReceiveResponse(request, nullptr)) {
			const DWORD error = GetLastError();
			if (cancelRequested_.load())
				throw std::runtime_error(kCancelledMessage);
			if (timeoutRequested_.load())
				throw std::runtime_error(
					RequestTimeoutMessage(settings.requestTimeoutSeconds));
			if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
				throw std::runtime_error(kCancelledMessage);
			throw std::runtime_error(DescribeWindowsNetworkError(error, "HTTP 请求"));
		}
		publish(AnswerEventType::Progress, "服务已连接，正在等待模型响应");

		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		if (!WinHttpQueryHeaders(request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
			WINHTTP_NO_HEADER_INDEX))
			throw std::runtime_error(WindowsError("读取 HTTP 状态码"));
		Logger::Write(LogLevel::Info,
			"AI HTTP status " + std::to_string(status));
		if (status >= 200 && status < 300)
			publish(AnswerEventType::Progress);

		std::string responseBytes;
		bool doneMarker = false;
		bool emittedContent = false;
		std::string reasoningPreview;
		ULONGLONG lastReasoningPublishTick = 0;
		std::string streamError;
		SseParser parser([&](std::string_view data) {
			if (data == "[DONE]") {
				doneMarker = true;
				return;
			}
			const json event = json::parse(data);
			if (event.contains("error")) {
				streamError = ExtractErrorMessage(std::string(data));
				if (streamError.empty())
					streamError = "服务返回流式错误";
				return;
			}
			if (!event.contains("choices") || !event["choices"].is_array() ||
				event["choices"].empty())
				return;
			const auto& choice = event["choices"][0];
			if (!choice.contains("delta") || !choice["delta"].is_object())
				return;
			const auto& delta = choice["delta"];
			if (!emittedContent) {
				const std::string reasoning = ExtractReasoningContent(delta);
				if (!reasoning.empty()) {
					AppendReasoningPreview(reasoningPreview, reasoning);
					const ULONGLONG now = GetTickCount64();
					if (lastReasoningPublishTick == 0 ||
						now - lastReasoningPublishTick >= 250) {
						publish(AnswerEventType::Progress, reasoningPreview);
						lastReasoningPublishTick = now;
					}
				}
			}
			if (!delta.contains("content"))
				return;
			std::string content = ExtractContent(delta["content"]);
			if (!content.empty()) {
				emittedContent = true;
				publish(AnswerEventType::Delta, std::move(content));
			}
		});

		for (;;) {
			throwIfTimedOut();
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request, &available)) {
				const DWORD error = GetLastError();
				if (cancelRequested_.load())
					throw std::runtime_error(kCancelledMessage);
				if (timeoutRequested_.load())
					throw std::runtime_error(
						RequestTimeoutMessage(settings.requestTimeoutSeconds));
				if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
					throw std::runtime_error(kCancelledMessage);
				throw std::runtime_error(DescribeWindowsNetworkError(error, "读取响应"));
			}
			if (!available)
				break;
			std::string chunk(available, '\0');
			DWORD read = 0;
			if (!WinHttpReadData(request, chunk.data(), available, &read)) {
				const DWORD error = GetLastError();
				if (cancelRequested_.load())
					throw std::runtime_error(kCancelledMessage);
				if (timeoutRequested_.load())
					throw std::runtime_error(
						RequestTimeoutMessage(settings.requestTimeoutSeconds));
				if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
					throw std::runtime_error(kCancelledMessage);
				throw std::runtime_error(DescribeWindowsNetworkError(error, "读取响应内容"));
			}
			chunk.resize(read);
			responseBytes += chunk;
			if (status >= 200 && status < 300)
				parser.Feed(chunk);
			if (responseBytes.size() > 8 * 1024 * 1024 && emittedContent)
				responseBytes.clear();
		}
		if (status >= 200 && status < 300)
			parser.Finish();

		ReleaseActiveRequest(request);
		request = nullptr;
		if (status < 200 || status >= 300) {
			std::string detail = ExtractErrorMessage(responseBytes);
			std::string message = "HTTP " + std::to_string(status);
			if (!detail.empty())
				message += ": " + detail;
			throw std::runtime_error(message);
		}
		if (!streamError.empty())
			throw std::runtime_error(streamError);

		if (!emittedContent && !responseBytes.empty() && !doneMarker) {
			const json response = json::parse(responseBytes);
			if (response.contains("choices") && response["choices"].is_array() &&
				!response["choices"].empty()) {
				const auto& choice = response["choices"][0];
				if (choice.contains("message") && choice["message"].contains("content")) {
					std::string content = ExtractContent(choice["message"]["content"]);
					if (!content.empty()) {
						emittedContent = true;
						publish(AnswerEventType::Delta, std::move(content));
					}
				}
			}
		}
		if (!emittedContent) {
			throw std::runtime_error(
				"服务返回 HTTP 200，但没有可显示的答案。请检查 API Base URL 是否为 "
				"OpenAI-compatible 地址，以及所选模型是否支持视觉输入");
		}
		if (timeoutWatchdog)
			timeoutWatchdog->Stop();
		throwIfTimedOut();
		publish(AnswerEventType::Completed);
		Logger::Write(LogLevel::Info, "AI request completed");
	}
	catch (const std::exception& exception) {
		if (cancelRequested_.load() || std::string_view(exception.what()) == kCancelledMessage) {
			publish(AnswerEventType::Cancelled);
			Logger::Write(LogLevel::Info, "AI request cancelled");
		}
		else if (timeoutRequested_.load() ||
			(timeoutAt && GetTickCount64() >= timeoutAt)) {
			timeoutRequested_.store(true);
			publish(AnswerEventType::Failed,
				RequestTimeoutMessage(settings.requestTimeoutSeconds));
			Logger::Write(LogLevel::Warning, "AI request timed out");
		}
		else {
			publish(AnswerEventType::Failed, exception.what());
			Logger::Write(LogLevel::Error, "AI request failed");
		}
	}

	HINTERNET request = nullptr;
	{
		std::scoped_lock lock(requestMutex_);
		request = activeRequest_;
		activeRequest_ = nullptr;
	}
	if (request)
		WinHttpCloseHandle(request);
	finish();
}

} // namespace coolhelper
