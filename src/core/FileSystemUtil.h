#pragma once

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
    const std::filesystem::path fileName(name);
    return !name.empty() && fileName == fileName.filename() && name != L"." &&
           name != L".." && name.find_first_of(L"<>:\"/\\|?*") == std::wstring_view::npos;
}

} // namespace rivan::core
