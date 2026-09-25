#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coolhelper {

struct ScreenshotResult {
	std::vector<std::uint8_t> png;
	int width = 0;
	int height = 0;
	std::string error;

	explicit operator bool() const noexcept { return !png.empty(); }
};

ScreenshotResult CapturePrimaryDisplayPng() noexcept;
std::string Base64Encode(const std::vector<std::uint8_t>& bytes) noexcept;

} // namespace coolhelper

