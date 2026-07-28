#pragma once

#include <Windows.h>

#include <limits>
#include <string>
#include <string_view>

namespace rivan::core {

[[nodiscard]] inline std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), required,
                            nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

[[nodiscard]] inline std::wstring Utf8ToWide(std::string_view text,
                                             std::wstring_view fallback = {}) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::wstring(fallback);
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return std::wstring(fallback);
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), required) != required) {
        return std::wstring(fallback);
    }
    return result;
}

} // namespace rivan::core
