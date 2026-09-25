#include "coolhelper/OverlayIpc.h"

#include "coolhelper/Logger.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>

namespace coolhelper {
namespace {

using overlayipc::IpcMessageHeader;
using overlayipc::IpcSharedHeader;

constexpr UINT32 kIpcMessageMaxPayload = overlayipc::kIpcMaxPayload;
constexpr UINT64 kIpcSectionSize = sizeof(IpcSharedHeader) +
	overlayipc::kIpcSlotCount * overlayipc::kIpcSlotSize;
constexpr UINT64 kAnswerEventQueueLimit = 4096;

UINT32 Utf8SafeChunkSize(std::string_view payload) noexcept {
	std::size_t chunk = std::min<std::size_t>(
		payload.size(), kIpcMessageMaxPayload);
	if (chunk == payload.size())
		return static_cast<UINT32>(chunk);
	// Do not leave half of a multi-byte character in either IPC message. This
	// also keeps the visible prefix valid if a later continuation is dropped.
	while (chunk > 0 &&
		(static_cast<unsigned char>(payload[chunk]) & 0xC0u) == 0x80u)
		--chunk;
	return static_cast<UINT32>(chunk != 0 ? chunk : kIpcMessageMaxPayload);
}

// dwm.exe runs under the per-session DWM-x virtual account, so every named
// object must be creatable/openable by it: a NULL DACL grants everyone access.
bool MakeOpenSecurityAttributes(SECURITY_ATTRIBUTES* attributes) noexcept {
	attributes->nLength = sizeof(*attributes);
	attributes->bInheritHandle = FALSE;
	auto* descriptor = new (std::nothrow) SECURITY_DESCRIPTOR();
	if (!descriptor)
		return false;
	if (!InitializeSecurityDescriptor(descriptor, SECURITY_DESCRIPTOR_REVISION) ||
		!SetSecurityDescriptorDacl(descriptor, TRUE, nullptr, FALSE)) {
		delete descriptor;
		return false;
	}
	attributes->lpSecurityDescriptor = descriptor;
	return true;
}

// The descriptor is freed by the caller after Create*/Open* returns.
void FreeSecurityDescriptor(SECURITY_ATTRIBUTES* attributes) noexcept {
	delete static_cast<SECURITY_DESCRIPTOR*>(attributes->lpSecurityDescriptor);
	attributes->lpSecurityDescriptor = nullptr;
}

UINT32 AnswerEventTypeToIpc(AnswerEventType type) noexcept {
	// The enum orders match by design; keep the mapping explicit for safety.
	switch (type) {
	case AnswerEventType::Started: return 0;
	case AnswerEventType::Delta: return 1;
	case AnswerEventType::Completed: return 2;
	case AnswerEventType::Failed: return 3;
	case AnswerEventType::Cancelled: return 4;
	case AnswerEventType::Cleared: return 5;
	case AnswerEventType::Progress: return 6;
	}
	return 1;
}

} // namespace

OverlayIpcSink::~OverlayIpcSink() {
	Stop();
}

bool OverlayIpcSink::Start(std::string& error) noexcept {
	if (running_.load())
		return true;

	SECURITY_ATTRIBUTES security = {};
	if (!MakeOpenSecurityAttributes(&security)) {
		error = "无法创建 IPC 安全描述符";
		return false;
	}

	const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security,
		PAGE_READWRITE,
		static_cast<DWORD>(kIpcSectionSize >> 32),
		static_cast<DWORD>(kIpcSectionSize & 0xFFFFFFFFu),
		overlayipc::kIpcSectionName);
	FreeSecurityDescriptor(&security);
	const DWORD mappingError = GetLastError();
	if (!mapping) {
		error = "无法创建共享内存：" + std::to_string(mappingError);
		Logger::Write(LogLevel::Error, "Overlay IPC mapping creation failed");
		return false;
	}
	if (mappingError == ERROR_ALREADY_EXISTS)
		Logger::Write(LogLevel::Warning,
			"Overlay IPC mapping already existed; reusing it");

	auto* header = static_cast<IpcSharedHeader*>(MapViewOfFile(
		mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
	if (!header) {
		CloseHandle(mapping);
		error = "无法映射共享内存：" + std::to_string(GetLastError());
		return false;
	}

	SECURITY_ATTRIBUTES eventSecurity = {};
	if (!MakeOpenSecurityAttributes(&eventSecurity)) {
		UnmapViewOfFile(header);
		CloseHandle(mapping);
		error = "无法创建 IPC 安全描述符";
		return false;
	}
	const HANDLE readyEvent = CreateEventW(&eventSecurity, FALSE, FALSE,
		overlayipc::kIpcReadyEventName);
	FreeSecurityDescriptor(&eventSecurity);
	if (!readyEvent) {
		UnmapViewOfFile(header);
		CloseHandle(mapping);
		error = "无法创建 IPC 通知事件：" + std::to_string(GetLastError());
		return false;
	}

	const HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	const HANDLE notifyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!stopEvent || !notifyEvent) {
		if (stopEvent) CloseHandle(stopEvent);
		if (notifyEvent) CloseHandle(notifyEvent);
		CloseHandle(readyEvent);
		UnmapViewOfFile(header);
		CloseHandle(mapping);
		error = "无法创建 IPC 内部事件：" + std::to_string(GetLastError());
		return false;
	}

	// Initialize the header for a fresh session.
	ZeroMemory(header, sizeof(*header));
	header->protocolVersion = overlayipc::kIpcProtocolVersion;
	header->headerSize = sizeof(IpcSharedHeader);
	header->slotCount = overlayipc::kIpcSlotCount;
	header->slotSize = overlayipc::kIpcSlotSize;
	header->slotDataOffset = sizeof(IpcSharedHeader);
	header->hubHeartbeatTick = GetTickCount64();

	mapping_ = mapping;
	header_ = header;
	readyEvent_ = readyEvent;
	stopEvent_ = stopEvent;
	notifyEvent_ = notifyEvent;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		queue_.clear();
		queueDropped_ = false;
	}

	const HANDLE thread = CreateThread(nullptr, 0, SenderProc, this, 0, nullptr);
	if (!thread) {
		CloseHandle(stopEvent_);
		CloseHandle(notifyEvent_);
		CloseHandle(readyEvent_);
		UnmapViewOfFile(header_);
		CloseHandle(mapping_);
		header_ = nullptr;
		mapping_ = nullptr;
		readyEvent_ = stopEvent_ = notifyEvent_ = nullptr;
		error = "无法创建 IPC 发送线程：" + std::to_string(GetLastError());
		return false;
	}
	senderThread_ = thread;
	running_.store(true);
	Logger::Write(LogLevel::Info, "Overlay IPC sink started");
	return true;
}

void OverlayIpcSink::Stop() noexcept {
	if (!running_.exchange(false) && !senderThread_)
		return;
	if (stopEvent_)
		SetEvent(stopEvent_);
	if (notifyEvent_)
		SetEvent(notifyEvent_);
	if (senderThread_) {
		WaitForSingleObject(senderThread_, 3000);
		CloseHandle(senderThread_);
		senderThread_ = nullptr;
	}
	if (readyEvent_) {
		CloseHandle(readyEvent_);
		readyEvent_ = nullptr;
	}
	if (notifyEvent_) {
		CloseHandle(notifyEvent_);
		notifyEvent_ = nullptr;
	}
	if (stopEvent_) {
		CloseHandle(stopEvent_);
		stopEvent_ = nullptr;
	}
	if (header_) {
		UnmapViewOfFile(header_);
		header_ = nullptr;
	}
	if (mapping_) {
		CloseHandle(mapping_);
		mapping_ = nullptr;
	}
	Logger::Write(LogLevel::Info, "Overlay IPC sink stopped");
}

bool OverlayIpcSink::HeartbeatFresh(UINT64 tick) noexcept {
	const UINT64 now = GetTickCount64();
	return tick != 0 && now >= tick && now - tick < overlayipc::kIpcHeartbeatTimeoutMs;
}

bool OverlayIpcSink::IsConnected() const noexcept {
	const IpcSharedHeader* header = header_;
	if (!header)
		return false;
	return HeartbeatFresh(header->dllHeartbeatTick);
}

const char* OverlayIpcSink::DescribeStatus() const noexcept {
	if (!running_.load())
		return "IPC 未启动";
	return IsConnected() ? "已连接 DWM 覆盖层" : "等待 DWM 覆盖层连接";
}

void OverlayIpcSink::SetRequestIdFilter(std::uint64_t requestId) noexcept {
	requestIdFilter_.store(requestId, std::memory_order_release);
}

void OverlayIpcSink::Publish(const AnswerEvent& event) noexcept {
	if (!running_.load())
		return;
	const std::uint64_t filter =
		requestIdFilter_.load(std::memory_order_acquire);
	if (filter != 0 && event.requestId != filter)
		return;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		if (queue_.size() >= kAnswerEventQueueLimit) {
			queue_.pop_front();
			queueDropped_ = true;
		}
		queue_.push_back(event);
	}
	if (notifyEvent_)
		SetEvent(notifyEvent_);
}

DWORD WINAPI OverlayIpcSink::SenderProc(void* context) noexcept {
	static_cast<OverlayIpcSink*>(context)->SenderLoop();
	return 0;
}

void OverlayIpcSink::SenderLoop() noexcept {
	UINT32 wireEpoch = 0;
	UINT32 wireSequence = 0;
	for (;;) {
		if (WaitForSingleObject(notifyEvent_, 1000) == WAIT_FAILED)
			return;
		if (!running_.load())
			break;
		if (stopEvent_ && WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0)
			break;

		if (header_)
			header_->hubHeartbeatTick = GetTickCount64();

		std::deque<AnswerEvent> batch;
		bool dropped = false;
		{
			std::lock_guard<std::mutex> lock(queueMutex_);
			batch.swap(queue_);
			dropped = queueDropped_;
			queueDropped_ = false;
		}

		bool wroteAny = false;
		while (!batch.empty()) {
			const AnswerEvent event = batch.front();
			batch.pop_front();
			// While the DLL is not listening there is no point in queueing
			// stale text: drop everything so a fresh attach starts clean.
			if (!IsConnected())
				continue;
			const UINT32 type = AnswerEventTypeToIpc(event.type);
			const UINT32 epoch = static_cast<UINT32>(event.requestId);
			if (epoch != wireEpoch) {
				wireEpoch = epoch;
				wireSequence = 0;
			}
			// Progress is replaceable transient state. Keep it outside the wire
			// sequence so a throttled/dropped preview can never look like lost
			// answer content to the overlay consumer.
			const UINT32 sequence = event.type == AnswerEventType::Progress
				? wireSequence : ++wireSequence;
			if (WriteMessage(type, epoch, sequence, event.payload))
				wroteAny = true;
		}
		if (dropped)
			Logger::Write(LogLevel::Warning,
				"Overlay IPC queue overflow; oldest events dropped");
		if (wroteAny && readyEvent_)
			SetEvent(readyEvent_);
	}
}

bool OverlayIpcSink::WriteMessage(
	UINT32 type, UINT32 epoch, UINT32 sequence,
	std::string_view payload) noexcept {
	return WriteMessage(type, epoch, sequence, payload, 0);
}

bool OverlayIpcSink::WriteMessage(
	UINT32 type, UINT32 epoch, UINT32 sequence,
	std::string_view payload, UINT32 flags) noexcept {
	IpcSharedHeader* header = header_;
	if (!header)
		return false;

	const UINT64 write = header->writeIndex;
	const UINT64 read = header->readIndex;
	if (write - read >= overlayipc::kIpcSlotCount) {
		// The consumer drains at compositor speed; give it a short grace
		// period, then drop this message rather than stall the stream.
		const ULONGLONG deadline = GetTickCount64() + 300;
		while (GetTickCount64() < deadline &&
			header->writeIndex - header->readIndex >= overlayipc::kIpcSlotCount)
			Sleep(2);
		if (header->writeIndex - header->readIndex >= overlayipc::kIpcSlotCount) {
			InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(
				&header->droppedByWriter));
			Logger::Write(LogLevel::Warning,
				"Overlay IPC ring full; message dropped");
			return false;
		}
	}

	const UINT32 chunk = Utf8SafeChunkSize(payload);
	UINT8* slot = reinterpret_cast<UINT8*>(header) + sizeof(IpcSharedHeader) +
		(write & (overlayipc::kIpcSlotCount - 1)) * overlayipc::kIpcSlotSize;
	auto* messageHeader = reinterpret_cast<IpcMessageHeader*>(slot);
	messageHeader->payloadSize = chunk;
	messageHeader->type = type;
	messageHeader->epoch = epoch;
	messageHeader->sequence = sequence;
	messageHeader->flags = flags;
	messageHeader->reserved = 0;
	if (chunk > 0)
		memcpy(slot + overlayipc::kIpcMessageHeaderSize, payload.data(), chunk);
	MemoryBarrier();
	InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&header->writeIndex));

	if (payload.size() > chunk) {
		// Oversized payloads continue in follow-up messages with the same
		// epoch/sequence and the continuation flag set; the consumer
		// concatenates them in order.
		return WriteMessage(type, epoch, sequence,
			payload.substr(chunk), overlayipc::kIpcMessageFlagContinuation);
	}
	return true;
}

} // namespace coolhelper
