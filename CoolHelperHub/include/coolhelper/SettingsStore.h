#pragma once

#include <filesystem>
#include <string>

#include "AppTypes.h"

namespace coolhelper {

class SettingsStore final {
public:
	SettingsStore();
	explicit SettingsStore(std::filesystem::path path);

	bool Load(AppSettings& settings, std::string& error) const noexcept;
	bool Save(const AppSettings& settings, std::string& error) const noexcept;
	const std::filesystem::path& Path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
};

} // namespace coolhelper
