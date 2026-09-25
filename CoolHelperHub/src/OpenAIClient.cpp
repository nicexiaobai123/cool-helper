#include "coolhelper/OpenAIClient.h"

#include "coolhelper/Logger.h"
#include "coolhelper/Screenshot.h"
#include "coolhelper/SseParser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace coolhelper {
namespace {

using nlohmann::json;

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

std::string WindowsError(const char* action) {
	return std::string(action) + "失败，Windows 错误码 " +
		std::to_string(GetLastError());
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

	// This is a transient one-line tail, not stored chain-of-thought. Keep it
	// compact and trim only at a UTF-8 code-point boundary.
	constexpr std::size_t kPreviewByteLimit = 360;
	if (preview.size() <= kPreviewByteLimit)
		return;
	std::size_t start = preview.size() - kPreviewByteLimit;
	while (start < preview.size() &&
		(static_cast<unsigned char>(preview[start]) & 0xC0u) == 0x80u)
		++start;
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
	HINTERNET request = nullptr;
	{
		std::scoped_lock lock(requestMutex_);
		request = activeRequest_;
		activeRequest_ = nullptr;
	}
	if (request)
		WinHttpCloseHandle(request);
	if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
		worker_.join();
	running_.store(false);
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

	try {
		publish(AnswerEventType::Started);
		Logger::Write(LogLevel::Info, "AI request started");
		const std::string imageBase64 = Base64Encode(png);
		png.clear();
		png.shrink_to_fit();
		if (imageBase64.empty())
			throw std::runtime_error("截图 Base64 编码失败");

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

		InternetHandle session(WinHttpOpen(L"CoolHelperHub/0.1",
			WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0));
		if (!session)
			throw std::runtime_error(WindowsError("WinHttpOpen"));
		WinHttpSetTimeouts(session, 10000, 10000, 30000, 120000);
		InternetHandle connection(WinHttpConnect(
			session, host.c_str(), components.nPort, 0));
		if (!connection)
			throw std::runtime_error(WindowsError("WinHttpConnect"));

		const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
			? WINHTTP_FLAG_SECURE : 0;
		HINTERNET request = WinHttpOpenRequest(connection, L"POST", path.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		if (!request)
			throw std::runtime_error(WindowsError("WinHttpOpenRequest"));
		SetActiveRequest(request);

		const std::wstring headers =
			L"Content-Type: application/json\r\nAccept: text/event-stream\r\n"
			L"Authorization: Bearer " + Utf8ToWide(settings.apiKey) + L"\r\n";
		if (cancelRequested_.load())
			throw std::runtime_error("请求已取消");
		if (!WinHttpSendRequest(request, headers.c_str(),
			static_cast<DWORD>(headers.size()),
			const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
			static_cast<DWORD>(body.size()), 0) ||
			!WinHttpReceiveResponse(request, nullptr)) {
			if (cancelRequested_.load())
				throw std::runtime_error("请求已取消");
			throw std::runtime_error(WindowsError("HTTP 请求"));
		}

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
			if (cancelRequested_.load())
				throw std::runtime_error("请求已取消");
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request, &available)) {
				if (cancelRequested_.load())
					throw std::runtime_error("请求已取消");
				throw std::runtime_error(WindowsError("读取响应"));
			}
			if (!available)
				break;
			std::string chunk(available, '\0');
			DWORD read = 0;
			if (!WinHttpReadData(request, chunk.data(), available, &read)) {
				if (cancelRequested_.load())
					throw std::runtime_error("请求已取消");
				throw std::runtime_error(WindowsError("读取响应内容"));
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
		publish(AnswerEventType::Completed);
		Logger::Write(LogLevel::Info, "AI request completed");
	}
	catch (const std::exception& exception) {
		if (cancelRequested_.load() || std::string_view(exception.what()) == "请求已取消") {
			publish(AnswerEventType::Cancelled);
			Logger::Write(LogLevel::Info, "AI request cancelled");
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
