#include <WinSock2.h>
#include <WS2tcpip.h>

#include "coolhelper/AnswerSink.h"
#include "coolhelper/OpenAIClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class TestSink final : public coolhelper::IAnswerSink {
public:
	void Publish(const coolhelper::AnswerEvent& event) noexcept override {
		std::scoped_lock lock(mutex_);
		events_.push_back(event);
		condition_.notify_all();
	}

	bool WaitForTerminal() {
		std::unique_lock lock(mutex_);
		return condition_.wait_for(lock, std::chrono::seconds(5), [&] {
			if (events_.empty()) return false;
			const auto type = events_.back().type;
			return type == coolhelper::AnswerEventType::Completed ||
				type == coolhelper::AnswerEventType::Failed ||
				type == coolhelper::AnswerEventType::Cancelled;
		});
	}

	std::vector<coolhelper::AnswerEvent> Events() const {
		std::scoped_lock lock(mutex_);
		return events_;
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable condition_;
	std::vector<coolhelper::AnswerEvent> events_;
};

bool SendAll(SOCKET socket, const std::string& data) {
	std::size_t offset = 0;
	while (offset < data.size()) {
		const int sent = send(socket, data.data() + offset,
			static_cast<int>(data.size() - offset), 0);
		if (sent <= 0)
			return false;
		offset += static_cast<std::size_t>(sent);
	}
	return true;
}

class MockServer final {
public:
	MockServer(int status, std::vector<std::string> bodyChunks, int bodyDelayMs = 0)
		: status_(status), bodyChunks_(std::move(bodyChunks)),
		bodyDelayMs_(bodyDelayMs) {
		listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener_ == INVALID_SOCKET)
			return;
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = 0;
		if (bind(listener_, reinterpret_cast<sockaddr*>(&address),
			sizeof(address)) == SOCKET_ERROR || listen(listener_, 1) == SOCKET_ERROR)
			return;
		int addressSize = sizeof(address);
		if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
			&addressSize) == SOCKET_ERROR)
			return;
		port_ = ntohs(address.sin_port);
		thread_ = std::thread(&MockServer::Run, this);
	}

	~MockServer() {
		if (listener_ != INVALID_SOCKET)
			closesocket(listener_);
		if (thread_.joinable())
			thread_.join();
	}

	unsigned short Port() const noexcept { return port_; }
	bool SawVisionRequest() const noexcept { return sawVisionRequest_.load(); }
	bool WaitForClient() const noexcept {
		for (int attempt = 0; attempt < 200; ++attempt) {
			if (accepted_.load()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}

private:
	void Run() {
		const SOCKET client = accept(listener_, nullptr, nullptr);
		if (client == INVALID_SOCKET)
			return;
		accepted_.store(true);
		std::string request;
		std::size_t expectedSize = 0;
		for (;;) {
			char buffer[4096] = {};
			const int received = recv(client, buffer, sizeof(buffer), 0);
			if (received <= 0)
				break;
			request.append(buffer, static_cast<std::size_t>(received));
			const auto headerEnd = request.find("\r\n\r\n");
			if (headerEnd != std::string::npos && !expectedSize) {
				const auto header = request.substr(0, headerEnd);
				const auto lengthAt = header.find("Content-Length:");
				if (lengthAt != std::string::npos) {
					const auto valueAt = lengthAt + std::string("Content-Length:").size();
					expectedSize = headerEnd + 4 +
						static_cast<std::size_t>(std::stoul(header.substr(valueAt)));
				}
			}
			if (expectedSize && request.size() >= expectedSize)
				break;
		}
		sawVisionRequest_ = request.find("POST /v1/chat/completions") != std::string::npos &&
			request.find("data:image/png;base64,") != std::string::npos &&
			request.find("\"stream\":true") != std::string::npos;

		std::size_t contentLength = 0;
		for (const auto& chunk : bodyChunks_)
			contentLength += chunk.size();
		const char* reason = status_ == 200 ? "OK" : "Error";
		const std::string headers = "HTTP/1.1 " + std::to_string(status_) + ' ' +
			reason + "\r\nContent-Type: " +
			(status_ == 200 ? "text/event-stream" : "application/json") +
			"\r\nContent-Length: " + std::to_string(contentLength) +
			"\r\nConnection: close\r\n\r\n";
		SendAll(client, headers);
		if (bodyDelayMs_ > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(bodyDelayMs_));
		for (const auto& chunk : bodyChunks_) {
			SendAll(client, chunk);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		shutdown(client, SD_BOTH);
		closesocket(client);
	}

	SOCKET listener_ = INVALID_SOCKET;
	unsigned short port_ = 0;
	int status_ = 0;
	std::vector<std::string> bodyChunks_;
	std::thread thread_;
	std::atomic_bool sawVisionRequest_ = false;
	std::atomic_bool accepted_ = false;
	int bodyDelayMs_ = 0;
};

coolhelper::AppSettings TestSettings(unsigned short port) {
	coolhelper::AppSettings settings;
	settings.apiBaseUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1/";
	settings.apiKey = "local-test-key";
	settings.model = "mock-vision";
	return settings;
}

bool TestStreaming() {
	MockServer server(200, {
		"data: {\"choices\":[{\"delta\":{\"content\":\"你",
		"好\"}}]}\n\n",
		"data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n",
		"data: [DONE]\n\n"
	});
	if (!server.Port())
		return false;
	TestSink sink;
	coolhelper::OpenAIClient client;
	const std::vector<std::uint8_t> dummyPng = {
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
	};
	if (!client.Start(TestSettings(server.Port()), dummyPng, 42, sink) ||
		!sink.WaitForTerminal())
		return false;
	client.Cancel();
	std::string answer;
	auto events = sink.Events();
	bool completed = false;
	std::uint64_t previous = 0;
	for (const auto& event : events) {
		if (event.sequence <= previous || event.requestId != 42)
			return false;
		previous = event.sequence;
		if (event.type == coolhelper::AnswerEventType::Delta)
			answer += event.payload;
		if (event.type == coolhelper::AnswerEventType::Completed)
			completed = true;
	}
	return completed && answer == "你好 world" && server.SawVisionRequest();
}

bool TestReasoningProgress() {
	MockServer server(200, {
		"data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789正在分析\\n复杂度\"}}]}\n\n",
		"data: {\"choices\":[{\"delta\":{\"content\":\"最终答案\"}}]}\n\n",
		"data: [DONE]\n\n"
	});
	if (!server.Port())
		return false;
	TestSink sink;
	coolhelper::OpenAIClient client;
	if (!client.Start(TestSettings(server.Port()), { 1, 2, 3 }, 43, sink) ||
		!sink.WaitForTerminal())
		return false;
	client.Cancel();
	bool sawWaiting = false;
	bool sawReasoning = false;
	std::string answer;
	for (const auto& event : sink.Events()) {
		if (event.type == coolhelper::AnswerEventType::Progress) {
			if (event.payload.empty())
				sawWaiting = true;
			else if (event.payload.size() <= 75 &&
				event.payload.find("正在分析 复杂度") != std::string::npos)
				sawReasoning = true;
		}
		else if (event.type == coolhelper::AnswerEventType::Delta) {
			answer += event.payload;
		}
	}
	return sawWaiting && sawReasoning && answer == "最终答案";
}

bool TestHttpError(int status) {
	MockServer server(status, {
		"{\"error\":{\"message\":\"mock failure\"}}"
	});
	if (!server.Port())
		return false;
	TestSink sink;
	coolhelper::OpenAIClient client;
	if (!client.Start(TestSettings(server.Port()), { 1, 2, 3 },
		static_cast<std::uint64_t>(status), sink) || !sink.WaitForTerminal())
		return false;
	client.Cancel();
	const auto events = sink.Events();
	return !events.empty() &&
		events.back().type == coolhelper::AnswerEventType::Failed &&
		events.back().payload.find("HTTP " + std::to_string(status)) != std::string::npos &&
		events.back().payload.find("mock failure") != std::string::npos;
}

bool TestSiliconFlowHttpError() {
	MockServer server(400, {
		"{\"code\":20012,\"message\":\"model does not support image input\",\"data\":\"\"}"
	});
	if (!server.Port())
		return false;
	TestSink sink;
	coolhelper::OpenAIClient client;
	if (!client.Start(TestSettings(server.Port()), { 1, 2, 3 }, 400, sink) ||
		!sink.WaitForTerminal())
		return false;
	client.Cancel();
	const auto events = sink.Events();
	return !events.empty() &&
		events.back().type == coolhelper::AnswerEventType::Failed &&
		events.back().payload.find("HTTP 400") != std::string::npos &&
		events.back().payload.find("model does not support image input") !=
			std::string::npos &&
		events.back().payload.find("code 20012") != std::string::npos;
}

bool TestEmptySuccessfulStream() {
	MockServer server(200, { "data: [DONE]\n\n" });
	if (!server.Port())
		return false;
	TestSink sink;
	coolhelper::OpenAIClient client;
	if (!client.Start(TestSettings(server.Port()), { 1, 2, 3 }, 200, sink) ||
		!sink.WaitForTerminal())
		return false;
	client.Cancel();
	const auto events = sink.Events();
	return !events.empty() &&
		events.back().type == coolhelper::AnswerEventType::Failed &&
		events.back().payload.find("HTTP 200") != std::string::npos &&
		events.back().payload.find("没有可显示的答案") != std::string::npos;
}

bool TestAnthropicUrlRejected() {
	try {
		(void)coolhelper::OpenAIClient::BuildChatCompletionsUrl(
			"https://api.z.ai/api/anthropic");
	}
	catch (const std::exception& exception) {
		return std::string(exception.what()).find("Anthropic") != std::string::npos;
	}
	return false;
}

bool TestCancellation() {
	MockServer server(200, {
		"data: {\"choices\":[{\"delta\":{\"content\":\"too late\"}}]}\n\n",
		"data: [DONE]\n\n"
	}, 500);
	if (!server.Port())
		return false;
	TestSink sink;
	coolhelper::OpenAIClient client;
	if (!client.Start(TestSettings(server.Port()), { 1, 2, 3 }, 99, sink) ||
		!server.WaitForClient())
		return false;
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	client.Cancel();
	const auto events = sink.Events();
	return !events.empty() &&
		events.back().type == coolhelper::AnswerEventType::Cancelled;
}

} // namespace

int main() {
	WSADATA data = {};
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return 1;
	const bool passed = TestStreaming() && TestReasoningProgress() &&
		TestHttpError(401) &&
		TestHttpError(429) && TestHttpError(500) &&
		TestSiliconFlowHttpError() && TestEmptySuccessfulStream() &&
		TestAnthropicUrlRejected() && TestCancellation();
	WSACleanup();
	if (!passed)
		std::cerr << "OpenAI-compatible WinHTTP integration test failed\n";
	return passed ? 0 : 1;
}
