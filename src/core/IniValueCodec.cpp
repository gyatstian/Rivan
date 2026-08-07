// IniValueCodec.cpp
#include "IniValueCodec.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <limits>

namespace rivan::core {

bool IsValidUtf8(std::string_view value) noexcept {
    if (value.empty()) return true;
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

std::optional<std::string> PathToUtf8(const std::filesystem::path& path) {
    const std::wstring native = path.native();
    if (native.empty()) return std::string{};
    if (native.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
                                             static_cast<int>(native.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0) return std::nullopt;
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
                            static_cast<int>(native.size()), result.data(), required,
                            nullptr, nullptr) != required) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::filesystem::path> PathFromUtf8(std::string_view value) {
    if (value.empty()) return std::filesystem::path{};
    if (value.find('\0') != std::string_view::npos ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required) != required) {
        return std::nullopt;
    }
    return std::filesystem::path(result);
}

std::string EncodeIniValue(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size());
    for (const unsigned char character : value) {
        const bool unreserved = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.' || character == '~';
        if (unreserved) {
            output.push_back(static_cast<char>(character));
        } else {
            output.push_back('%');
            output.push_back(hex[character >> 4U]);
            output.push_back(hex[character & 0x0FU]);
        }
    }
    return output;
}

std::optional<std::string> DecodeIniValue(std::string_view value, bool requireValidUtf8) {
    const auto hexDigit = [](char character) {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            output.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) return std::nullopt;
        const int high = hexDigit(value[index + 1]);
        const int low = hexDigit(value[index + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        output.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    if (requireValidUtf8 &&
        (output.find('\0') != std::string::npos || !IsValidUtf8(output))) {
        return std::nullopt;
    }
    return output;
}

} // namespace rivan::core
