// Internal SettingsManager persistence helpers.
#pragma once

#include "SettingsManager.h"

#include "../core/IniDocument.h"
#include "../core/IniValueCodec.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace rivan::config {

inline constexpr std::size_t kMaximumIdentifierBytes = 64;
inline constexpr std::size_t kMaximumSelectionBytes = 4096;
inline constexpr std::uint64_t kMaximumPositionMilliseconds =
    30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
// Collapsed modules can retain expanded pixel geometry beyond current normalized
// canvas bounds after a window shrink. Keep persisted values finite and bounded.
inline constexpr float kMaximumStoredModuleCoordinate = 256.0F;

inline void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

inline void AddWarning(std::string* warnings, std::string message) {
    if (warnings == nullptr) {
        return;
    }
    if (!warnings->empty()) {
        warnings->push_back('\n');
    }
    *warnings += std::move(message);
}

inline bool IsIdentifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumIdentifierBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               character == '-' || character == '_';
    });
}

template <typename Integer>
inline std::optional<Integer> ParseInteger(std::string_view value) noexcept {
    Integer result{};
    const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (status != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

inline std::optional<bool> ParseBool(std::string_view value) noexcept {
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    return std::nullopt;
}

inline std::string BoolText(bool value) {
    return value ? "true" : "false";
}

inline void ReadIntegerField(const core::IniDocument& document, std::string_view section,
                             std::string_view key, int minimum, int maximum, int& destination,
                             std::string* warnings) {
    const auto value = document.Get(section, key);
    if (!value) {
        return;
    }
    const auto parsed = ParseInteger<int>(*value);
    if (!parsed || *parsed < minimum || *parsed > maximum) {
        AddWarning(warnings, "Ignoring invalid " + std::string(section) + "." + std::string(key));
        return;
    }
    destination = *parsed;
}

inline void ReadBoolField(const core::IniDocument& document, std::string_view section,
                          std::string_view key, bool& destination, std::string* warnings) {
    const auto value = document.Get(section, key);
    if (!value) {
        return;
    }
    const auto parsed = ParseBool(*value);
    if (!parsed) {
        AddWarning(warnings, "Ignoring invalid " + std::string(section) + "." + std::string(key));
        return;
    }
    destination = *parsed;
}

inline void ReadFloatField(const core::IniDocument& document, std::string_view section,
                           std::string_view key, float minimum, float maximum, float& destination,
                           std::string* warnings) {
    const auto value = document.Get(section, key);
    if (!value) return;
    const std::string text(*value);
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed) ||
        parsed < minimum || parsed > maximum) {
        AddWarning(warnings, "Ignoring invalid " + std::string(section) + "." + std::string(key));
        return;
    }
    destination = parsed;
}

inline std::string FloatText(float value) {
    char buffer[32]{};
    // std::to_chars is locale-independent (always a '.' separator, unlike a
    // future setlocale(LC_ALL, "") + snprintf "%.6g") and emits the shortest
    // representation that round-trips the float. Greatest float needs 9 digits
    // plus exponent, so max_digits10 + 20 = 29 chars fits the buffer.
    const auto [end, status] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (status == std::errc{}) {
        return std::string(buffer, end);
    }
    // Fallback for toolchains without floating-point to_chars support; "%.9g"
    // still round-trips but follows the C locale.
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

inline void ReadEncodedString(const core::IniDocument& document, std::string_view section,
                              std::string_view key, std::size_t maximumBytes,
                              std::string& destination, std::string* warnings) {
    const auto value = document.Get(section, key);
    if (!value) {
        return;
    }
    const auto decoded = core::DecodeIniValue(*value);
    if (!decoded || decoded->size() > maximumBytes) {
        AddWarning(warnings, "Ignoring invalid " + std::string(section) + "." + std::string(key));
        return;
    }
    destination = *decoded;
}

inline std::optional<bool> FileIsMissing(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        SetError(error, "Unable to inspect " + path.string() + ": " + ec.message());
        return std::nullopt;
    }
    return !exists;
}

inline bool ValidateFormat(const core::IniDocument& document, std::string_view fileKind,
                           std::string* error) {
    const auto format = document.Get("meta", "format");
    if (!format || *format != "1") {
        SetError(error, std::string(fileKind) + " has an unsupported or missing format");
        return false;
    }
    return true;
}

} // namespace rivan::config
