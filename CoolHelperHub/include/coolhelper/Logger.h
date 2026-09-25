#pragma once

#include <string_view>

namespace coolhelper {

enum class LogLevel { Info, Warning, Error };

class Logger final {
public:
	static void Initialize() noexcept;
	static void Shutdown() noexcept;
	static void Write(LogLevel level, std::string_view message) noexcept;
};

} // namespace coolhelper

