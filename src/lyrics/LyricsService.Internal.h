// LyricsService.Internal.h
// Helpers shared between the LyricsService split translation units (core, parsing,
// network, storage). The public API stays in LyricsService.h.
#pragma once

#include "LyricsService.h"

#include "../core/Json.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace rivan::lyrics::detail {

inline constexpr std::size_t kMaximumResponseBytes = 1024U * 1024U;
// User-authored lyrics files: the ".txt" suffix keeps them out of the fingerprint cache
// (".lyrics") so cache pruning never deletes them and text editors open them directly.
inline constexpr std::wstring_view kCustomLyricsExtension = L".txt";
inline constexpr std::wstring_view kCustomLyricsMagic = L"#RIVAN-CUSTOM-LYRICS-1";
inline constexpr std::wstring_view kSongFileHeader = L"#Song file:";

[[nodiscard]] std::wstring Trim(std::wstring value);
[[nodiscard]] std::wstring FoldCase(std::wstring value);

[[nodiscard]] std::optional<std::string> JsonString(const core::JsonValue& object,
                                                    std::string_view field);
[[nodiscard]] std::optional<double> JsonNumber(const core::JsonValue& object,
                                               std::string_view field);
[[nodiscard]] std::wstring JsonWideString(const core::JsonValue& object,
                                          std::string_view field);

// Builds a lyrics document from one already-parsed lrclib.net response object (or the
// lyrics.ovh fallback object, which just reads "lyrics"). A missing field or an explicit
// null falls through to the next source exactly like the former text-scanning parser did.
[[nodiscard]] LyricsDocument ParseLrclibObject(const core::JsonValue& root);

[[nodiscard]] std::optional<double> ParseNumber(std::wstring_view value);

} // namespace rivan::lyrics::detail
