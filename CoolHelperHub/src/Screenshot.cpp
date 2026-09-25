#include "coolhelper/Screenshot.h"

#include <Windows.h>
#include <Wincrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>

namespace coolhelper {
namespace {

using Microsoft::WRL::ComPtr;

class ScreenDc final {
public:
	ScreenDc() : value_(GetDC(nullptr)) {}
	~ScreenDc() { if (value_) ReleaseDC(nullptr, value_); }
	operator HDC() const noexcept { return value_; }
private:
	HDC value_ = nullptr;
};

class MemoryDc final {
public:
	explicit MemoryDc(HDC source) : value_(CreateCompatibleDC(source)) {}
	~MemoryDc() { if (value_) DeleteDC(value_); }
	operator HDC() const noexcept { return value_; }
private:
	HDC value_ = nullptr;
};

class Bitmap final {
public:
	explicit Bitmap(HBITMAP value) : value_(value) {}
	~Bitmap() { if (value_) DeleteObject(value_); }
	operator HBITMAP() const noexcept { return value_; }
private:
	HBITMAP value_ = nullptr;
};

} // namespace

ScreenshotResult CapturePrimaryDisplayPng() noexcept {
	ScreenshotResult result;
	result.width = GetSystemMetrics(SM_CXSCREEN);
	result.height = GetSystemMetrics(SM_CYSCREEN);
	if (result.width <= 0 || result.height <= 0) {
		result.error = "无法获取主显示器尺寸";
		return result;
	}

	ScreenDc screen;
	if (!screen) {
		result.error = "GetDC 截图失败";
		return result;
	}
	MemoryDc memory(screen);
	if (!memory) {
		result.error = "CreateCompatibleDC 截图失败";
		return result;
	}
	Bitmap bitmap(CreateCompatibleBitmap(screen, result.width, result.height));
	if (!bitmap) {
		result.error = "CreateCompatibleBitmap 截图失败";
		return result;
	}
	const HGDIOBJ previous = SelectObject(memory, bitmap);
	const BOOL copied = BitBlt(memory, 0, 0, result.width, result.height,
		screen, 0, 0, SRCCOPY | CAPTUREBLT);
	SelectObject(memory, previous);
	if (!copied) {
		result.error = "BitBlt 截图失败";
		return result;
	}

	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
		CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
		result.error = "WIC 初始化失败";
		return result;
	}
	ComPtr<IWICBitmap> wicBitmap;
	if (FAILED(factory->CreateBitmapFromHBITMAP(
		bitmap, nullptr, WICBitmapIgnoreAlpha, &wicBitmap))) {
		result.error = "WIC 无法读取截图";
		return result;
	}
	ComPtr<IStream> stream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
		result.error = "无法创建 PNG 内存流";
		return result;
	}
	ComPtr<IWICBitmapEncoder> encoder;
	if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
		FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
		result.error = "PNG 编码器初始化失败";
		return result;
	}
	ComPtr<IWICBitmapFrameEncode> frame;
	ComPtr<IPropertyBag2> properties;
	if (FAILED(encoder->CreateNewFrame(&frame, &properties)) ||
		FAILED(frame->Initialize(properties.Get())) ||
		FAILED(frame->SetSize(
			static_cast<UINT>(result.width), static_cast<UINT>(result.height)))) {
		result.error = "PNG 帧初始化失败";
		return result;
	}
	WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
	if (FAILED(frame->SetPixelFormat(&format)) ||
		FAILED(frame->WriteSource(wicBitmap.Get(), nullptr)) ||
		FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
		result.error = "PNG 编码失败";
		return result;
	}

	STATSTG statistics = {};
	HGLOBAL global = nullptr;
	if (FAILED(stream->Stat(&statistics, STATFLAG_NONAME)) ||
		statistics.cbSize.QuadPart <= 0 ||
		statistics.cbSize.QuadPart > std::numeric_limits<SIZE_T>::max() ||
		FAILED(GetHGlobalFromStream(stream.Get(), &global))) {
		result.error = "无法读取 PNG 内存流";
		return result;
	}
	const auto size = static_cast<SIZE_T>(statistics.cbSize.QuadPart);
	const void* bytes = GlobalLock(global);
	if (!bytes) {
		result.error = "无法锁定 PNG 内存";
		return result;
	}
	try {
		const auto begin = static_cast<const std::uint8_t*>(bytes);
		result.png.assign(begin, begin + size);
	}
	catch (...) {
		result.error = "PNG 内存不足";
	}
	GlobalUnlock(global);
	return result;
}

std::string Base64Encode(const std::vector<std::uint8_t>& bytes) noexcept {
	if (bytes.empty() || bytes.size() > MAXDWORD)
		return {};
	DWORD required = 0;
	if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()),
		CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &required))
		return {};
	try {
		std::string result(required, '\0');
		if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()),
			CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
			result.data(), &required))
			return {};
		if (!result.empty() && result.back() == '\0')
			result.pop_back();
		return result;
	}
	catch (...) {
		return {};
	}
}

} // namespace coolhelper

