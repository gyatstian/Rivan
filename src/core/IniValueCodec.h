// IniValueCodec.h
// UTF-8 and percent encoding helpers for values stored in INI documents.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rivan::core {

[[nodiscard]] bool IsValidUtf8(std::string_view value) noexcept;
[[nodiscard]] std::optional<std::string> PathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::filesystem::path> PathFromUtf8(std::string_view value);
[[nodiscard]] std::string EncodeIniValue(std::string_view value);
[[nodiscard]] std::optional<std::string> DecodeIniValue(std::string_view value,
                                                         bool requireValidUtf8 = true);

} // namespace rivan::core
