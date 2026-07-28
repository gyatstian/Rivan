// Rivan source file
// Purpose: Windows-native locations used by the application.
#pragma once

#include <filesystem>
#include <string>

namespace rivan::core {

class AppPaths final {
public:
    [[nodiscard]] static std::filesystem::path ExecutablePath();
    [[nodiscard]] static std::filesystem::path ExecutableDirectory();

    // Defaults to the user's Music known folder followed by "Rivan".
    [[nodiscard]] static std::filesystem::path DefaultMusicRoot();

    // Defaults to the user's LocalAppData known folder followed by "Rivan".
    [[nodiscard]] static std::filesystem::path LocalDataRoot();
    [[nodiscard]] static std::filesystem::path SettingsFile();
    [[nodiscard]] static std::filesystem::path SessionFile();
    [[nodiscard]] static std::filesystem::path SkinsDirectory();

    // Creates the writable application and skins directories.
    [[nodiscard]] static bool EnsureDirectories(std::wstring* error = nullptr);

private:
    AppPaths() = delete;
};

} // namespace rivan::core
