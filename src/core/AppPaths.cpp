// Rivan source file
// Purpose: Windows-native locations used by the application.
#include "AppPaths.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShlObj.h>

#include <system_error>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace rivan::core {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value);
    if (FAILED(result) || value == nullptr) {
        return {};
    }

    std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path;
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }

    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) {
        return {};
    }
    value.resize(copied);
    return std::filesystem::path(value);
}

void SetError(std::wstring* error, const std::wstring& message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

std::filesystem::path AppPaths::ExecutablePath() {
    std::wstring buffer(260, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path AppPaths::ExecutableDirectory() {
    return ExecutablePath().parent_path();
}

std::filesystem::path AppPaths::DefaultMusicRoot() {
    std::filesystem::path music = KnownFolder(FOLDERID_Music);
    if (music.empty()) {
        const auto profile = EnvironmentPath(L"USERPROFILE");
        if (!profile.empty()) {
            music = profile / L"Music";
        }
    }
    if (music.empty()) {
        music = ExecutableDirectory() / L"Music";
    }
    return music / L"Rivan";
}

std::filesystem::path AppPaths::LocalDataRoot() {
    std::filesystem::path local = KnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) {
        local = EnvironmentPath(L"LOCALAPPDATA");
    }
    if (local.empty()) {
        local = ExecutableDirectory();
    }
    return local / L"Rivan";
}

std::filesystem::path AppPaths::SettingsFile() {
    return LocalDataRoot() / L"settings.ini";
}

std::filesystem::path AppPaths::SessionFile() {
    return LocalDataRoot() / L"session.ini";
}

std::filesystem::path AppPaths::SkinsDirectory() {
    return LocalDataRoot() / L"skins";
}

bool AppPaths::EnsureDirectories(std::wstring* error) {
    std::error_code ec;
    std::filesystem::create_directories(LocalDataRoot(), ec);
    if (ec) {
        SetError(error, L"Unable to create application data directory: " +
                            std::filesystem::path(ec.message()).wstring());
        return false;
    }

    std::filesystem::create_directories(SkinsDirectory(), ec);
    if (ec) {
        SetError(error, L"Unable to create skins directory: " +
                            std::filesystem::path(ec.message()).wstring());
        return false;
    }

    std::filesystem::create_directories(LocalDataRoot() / L"tools", ec);
    if (ec) {
        SetError(error, L"Unable to create tools directory: " +
                            std::filesystem::path(ec.message()).wstring());
        return false;
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

} // namespace rivan::core
