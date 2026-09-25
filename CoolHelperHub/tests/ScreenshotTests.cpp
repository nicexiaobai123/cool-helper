#include "coolhelper/Screenshot.h"

#include <Windows.h>
#include <Wincrypt.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

int main() {
	const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
		std::cerr << "COM initialization failed\n";
		return 1;
	}

	const auto screenshot = coolhelper::CapturePrimaryDisplayPng();
	bool passed = static_cast<bool>(screenshot);
	const std::array<unsigned char, 8> signature = {
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
	};
	passed = passed && screenshot.width > 0 && screenshot.height > 0 &&
		screenshot.png.size() > signature.size() &&
		std::equal(signature.begin(), signature.end(), screenshot.png.begin());

	const std::string base64 = coolhelper::Base64Encode(screenshot.png);
	DWORD decodedSize = 0;
	passed = passed && base64.starts_with("iVBOR") &&
		CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()),
			CRYPT_STRING_BASE64, nullptr, &decodedSize, nullptr, nullptr) &&
		decodedSize == screenshot.png.size();

	if (!passed)
		std::cerr << "Screenshot validation failed: " << screenshot.error << '\n';
	if (SUCCEEDED(comResult))
		CoUninitialize();
	return passed ? 0 : 1;
}
