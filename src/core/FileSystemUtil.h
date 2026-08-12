#pragma once

#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace rivan::core {

[[nodiscard]] inline std::filesystem::path UniqueDestination(
    const std::filesystem::path& directory,
    const std::filesystem::path& fileName) {
    std::error_code ec;
    auto destination = directory / fileName;
    if (!std::filesystem::exists(destination, ec)) return destination;
    const auto stem = fileName.stem().wstring();
    const auto ext = fileName.extension().wstring();
    for (int index = 1; index < 1000; ++index) {
        destination = directory / (stem + L"-" + std::to_wstring(index) + ext);
        ec.clear();
        if (!std::filesystem::exists(destination, ec)) return destination;
    }
    return directory / (stem + L"-copy" + ext);
}

[[nodiscard]] inline bool IsValidFileName(std::wstring_view name) {
    if (name.empty() || name == L"." || name == L".." ||
        name.find_first_of(L"<>:\"/\\|?*") != std::wstring_view::npos) {
        return false;
    }
    const std::filesystem::path fileName(name);
    if (fileName != fileName.filename()) return false;
    // Windows strips trailing dots and spaces, so "foo." would alias "foo".
    if (name.back() == L'.' || name.back() == L' ') return false;

    // Reserved device names are rejected even with an extension ("CON.txt").
    const auto dot = name.find_first_of(L'.');
    std::wstring stem(name.substr(0, dot));
    for (auto& character : stem) {
        character = static_cast<wchar_t>(std::towupper(character));
    }
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) &&
         stem[3] >= L'1' && stem[3] <= L'9')) {
        return false;
    }
    return true;
}

} // namespace rivan::core
