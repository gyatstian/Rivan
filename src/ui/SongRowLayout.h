// SongRowLayout.h
// Durable, skin-independent geometry and styling for library song rows.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rivan::ui {

enum class SongRowField : std::uint8_t {
    Number,
    Title,
    Duration,
    Cover,
    Artist,
    Bitrate,
    Count,
};

enum class SongRowFontWeight : std::uint8_t {
    Normal,
    SemiBold,
    Bold,
};

enum class SongRowFontStyle : std::uint8_t {
    Normal,
    Italic,
};

enum class SongRowTextColor : std::uint8_t {
    Primary,
    Secondary,
};

enum class SongRowSnapSide : std::uint8_t {
    Left,
    Right,
};

inline constexpr std::size_t kSongRowFieldCount =
    static_cast<std::size_t>(SongRowField::Count);

inline constexpr int kSongRowDefaultSnapGapPixels = 1;
inline constexpr int kSongRowMinimumSnapGapPixels = -16;
inline constexpr int kSongRowMaximumSnapGapPixels = 16;

// A field remains attached to this target after the editor drag completes. This
// preserves chains (Number -> Cover -> Title) and sibling attachments (Cover ->
// Title and Cover -> Artist) as either field is later repositioned.
struct SongRowSnap final {
    SongRowField target{SongRowField::Number};
    SongRowSnapSide side{SongRowSnapSide::Right};
    int gapPixels{kSongRowDefaultSnapGapPixels};

    [[nodiscard]] constexpr bool operator==(const SongRowSnap&) const noexcept = default;
};

struct SongRowFieldLayout final {
    bool visible{true};
    // Normalized to the full row bounds. Geometry is resolved to pixels only by the
    // renderer, keeping persistence independent of window and module dimensions.
    float x{};
    float y{};
    float width{};
    float height{};
    // Applied to the active skin's base font size, never used as an absolute point size.
    int fontSizeDelta{};
    SongRowFontWeight fontWeight{SongRowFontWeight::Normal};
    SongRowFontStyle fontStyle{SongRowFontStyle::Normal};
    SongRowTextColor textColor{SongRowTextColor::Primary};
    // Text fields fit their rendered content by default. A manual resize turns
    // this off for that field and uses width as an explicit text box width.
    bool fluid{true};
    std::optional<SongRowSnap> snap;

    [[nodiscard]] constexpr bool operator==(const SongRowFieldLayout&) const noexcept = default;
};

struct SongRowLayout final {
    float rowHeight{36.0F};
    std::array<SongRowFieldLayout, kSongRowFieldCount> fields{};

    [[nodiscard]] SongRowFieldLayout& Field(SongRowField field) noexcept {
        return fields[static_cast<std::size_t>(field)];
    }

    [[nodiscard]] const SongRowFieldLayout& Field(SongRowField field) const noexcept {
        return fields[static_cast<std::size_t>(field)];
    }

    [[nodiscard]] static SongRowLayout Defaults() noexcept {
        SongRowLayout result;
        result.rowHeight = 36.0F;
        result.Field(SongRowField::Number) = {
            true, 0.00F, 0.10F, 0.090F, 0.80F, -1,
            SongRowFontWeight::Normal, SongRowFontStyle::Normal, SongRowTextColor::Secondary};
        result.Field(SongRowField::Cover) = {
            true, 0.105F, 0.10F, 0.090F, 0.80F, 0,
            SongRowFontWeight::Normal, SongRowFontStyle::Normal, SongRowTextColor::Primary};
        result.Field(SongRowField::Title) = {
            true, 0.215F, 0.03F, 0.520F, 0.53F, 0,
            SongRowFontWeight::SemiBold, SongRowFontStyle::Normal, SongRowTextColor::Primary};
        result.Field(SongRowField::Artist) = {
            true, 0.215F, 0.58F, 0.520F, 0.35F, -2,
            SongRowFontWeight::Normal, SongRowFontStyle::Normal, SongRowTextColor::Secondary};
        result.Field(SongRowField::Duration) = {
            true, 0.780F, 0.08F, 0.170F, 0.44F, -1,
            SongRowFontWeight::Normal, SongRowFontStyle::Normal, SongRowTextColor::Primary};
        result.Field(SongRowField::Bitrate) = {
            true, 0.780F, 0.54F, 0.170F, 0.32F, -3,
            SongRowFontWeight::Normal, SongRowFontStyle::Normal, SongRowTextColor::Secondary};
        result.Field(SongRowField::Cover).fluid = false;
        return result;
    }
};

[[nodiscard]] constexpr const char* SongRowFieldKey(SongRowField field) noexcept {
    switch (field) {
    case SongRowField::Number: return "number";
    case SongRowField::Title: return "title";
    case SongRowField::Duration: return "duration";
    case SongRowField::Cover: return "cover";
    case SongRowField::Artist: return "artist";
    case SongRowField::Bitrate: return "bitrate";
    case SongRowField::Count: break;
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<SongRowField> SongRowFieldFromKey(
    const std::string_view key) noexcept {
    if (key == "number") return SongRowField::Number;
    if (key == "title") return SongRowField::Title;
    if (key == "duration") return SongRowField::Duration;
    if (key == "cover") return SongRowField::Cover;
    if (key == "artist") return SongRowField::Artist;
    if (key == "bitrate") return SongRowField::Bitrate;
    return std::nullopt;
}

[[nodiscard]] constexpr const wchar_t* SongRowFieldName(SongRowField field) noexcept {
    switch (field) {
    case SongRowField::Number: return L"SONG NUMBER";
    case SongRowField::Title: return L"SONG NAME";
    case SongRowField::Duration: return L"SONG DURATION";
    case SongRowField::Cover: return L"COVER ARTWORK";
    case SongRowField::Artist: return L"AUTHOR NAME";
    case SongRowField::Bitrate: return L"BITRATE / KBPS";
    case SongRowField::Count: break;
    }
    return L"SONG FIELD";
}

} // namespace rivan::ui
