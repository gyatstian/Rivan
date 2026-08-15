// SettingsManager.Settings.cpp
// Durable application-preference serialization and validation.
#include "SettingsManager.Persistence.h"

#include "../core/AppPaths.h"
#include "../ui/SongRowLayoutGeometry.h"

namespace rivan::config {
namespace {

constexpr std::string_view kSongRowLayoutSection = "song_row_layout";

[[nodiscard]] const char* SongRowWeightText(const ui::SongRowFontWeight weight) noexcept {
    switch (weight) {
    case ui::SongRowFontWeight::Normal: return "normal";
    case ui::SongRowFontWeight::SemiBold: return "semibold";
    case ui::SongRowFontWeight::Bold: return "bold";
    }
    return "normal";
}

[[nodiscard]] std::optional<ui::SongRowFontWeight> ParseSongRowWeight(
    const std::string_view text) noexcept {
    if (text == "normal") return ui::SongRowFontWeight::Normal;
    if (text == "semibold") return ui::SongRowFontWeight::SemiBold;
    if (text == "bold") return ui::SongRowFontWeight::Bold;
    return std::nullopt;
}

[[nodiscard]] const char* SongRowStyleText(const ui::SongRowFontStyle style) noexcept {
    return style == ui::SongRowFontStyle::Italic ? "italic" : "normal";
}

[[nodiscard]] std::optional<ui::SongRowFontStyle> ParseSongRowStyle(
    const std::string_view text) noexcept {
    if (text == "normal") return ui::SongRowFontStyle::Normal;
    if (text == "italic") return ui::SongRowFontStyle::Italic;
    return std::nullopt;
}

[[nodiscard]] const char* SongRowColorText(const ui::SongRowTextColor color) noexcept {
    return color == ui::SongRowTextColor::Secondary ? "secondary" : "primary";
}

[[nodiscard]] std::optional<ui::SongRowTextColor> ParseSongRowColor(
    const std::string_view text) noexcept {
    if (text == "primary") return ui::SongRowTextColor::Primary;
    if (text == "secondary") return ui::SongRowTextColor::Secondary;
    return std::nullopt;
}

[[nodiscard]] const char* SongRowSnapSideText(const ui::SongRowSnapSide side) noexcept {
    return side == ui::SongRowSnapSide::Left ? "left" : "right";
}

[[nodiscard]] std::optional<ui::SongRowSnapSide> ParseSongRowSnapSide(
    const std::string_view text) noexcept {
    if (text == "left") return ui::SongRowSnapSide::Left;
    if (text == "right") return ui::SongRowSnapSide::Right;
    return std::nullopt;
}

void WriteSongRowLayout(core::IniDocument& document, const ui::SongRowLayout& layout) {
    document.Set(std::string(kSongRowLayoutSection), "version", "2");
    document.Set(std::string(kSongRowLayoutSection), "row_height",
                  FloatText(layout.rowHeight));
    for (std::size_t index = 0; index < ui::kSongRowFieldCount; ++index) {
        const auto field = static_cast<ui::SongRowField>(index);
        const auto& value = layout.Field(field);
        const std::string prefix = std::string("field_") + ui::SongRowFieldKey(field) + "_";
        document.Set(std::string(kSongRowLayoutSection), prefix + "visible", BoolText(value.visible));
        document.Set(std::string(kSongRowLayoutSection), prefix + "x", FloatText(value.x));
        document.Set(std::string(kSongRowLayoutSection), prefix + "y", FloatText(value.y));
        document.Set(std::string(kSongRowLayoutSection), prefix + "width", FloatText(value.width));
        document.Set(std::string(kSongRowLayoutSection), prefix + "height", FloatText(value.height));
        document.Set(std::string(kSongRowLayoutSection), prefix + "fluid", BoolText(value.fluid));
        document.Set(std::string(kSongRowLayoutSection), prefix + "font_size_delta",
                     std::to_string(value.fontSizeDelta));
        document.Set(std::string(kSongRowLayoutSection), prefix + "font_weight",
                     SongRowWeightText(value.fontWeight));
        document.Set(std::string(kSongRowLayoutSection), prefix + "font_style",
                     SongRowStyleText(value.fontStyle));
        document.Set(std::string(kSongRowLayoutSection), prefix + "text_color",
                      SongRowColorText(value.textColor));
        if (value.snap) {
            document.Set(std::string(kSongRowLayoutSection), prefix + "snap_target",
                         ui::SongRowFieldKey(value.snap->target));
            document.Set(std::string(kSongRowLayoutSection), prefix + "snap_side",
                         SongRowSnapSideText(value.snap->side));
            document.Set(std::string(kSongRowLayoutSection), prefix + "snap_gap",
                         std::to_string(value.snap->gapPixels));
        }
    }
}

[[nodiscard]] bool HasSongRowLayout(const core::IniDocument& document) noexcept {
    return document.Get(kSongRowLayoutSection, "version").has_value() ||
           document.Get(kSongRowLayoutSection, "row_height").has_value();
}

void ReadSongRowLayout(const core::IniDocument& document, ui::SongRowLayout& layout,
                       std::string* warnings) {
    int version = 1;
    if (const auto text = document.Get(kSongRowLayoutSection, "version")) {
        const auto parsed = ParseInteger<int>(*text);
        if (!parsed || (*parsed != 1 && *parsed != 2)) {
            AddWarning(warnings, "Ignoring unsupported song_row_layout.version");
            return;
        }
        version = *parsed;
    }
    ReadFloatField(document, kSongRowLayoutSection, "row_height", 20.0F, 160.0F,
                   layout.rowHeight, warnings);
    for (std::size_t index = 0; index < ui::kSongRowFieldCount; ++index) {
        const auto field = static_cast<ui::SongRowField>(index);
        auto& value = layout.Field(field);
        const std::string prefix = std::string("field_") + ui::SongRowFieldKey(field) + "_";
        ReadBoolField(document, kSongRowLayoutSection, prefix + "visible", value.visible, warnings);
        ReadFloatField(document, kSongRowLayoutSection, prefix + "x", 0.0F, 1.0F,
                       value.x, warnings);
        ReadFloatField(document, kSongRowLayoutSection, prefix + "y", 0.0F, 1.0F,
                       value.y, warnings);
        ReadFloatField(document, kSongRowLayoutSection, prefix + "width", 0.02F, 1.0F,
                       value.width, warnings);
        ReadFloatField(document, kSongRowLayoutSection, prefix + "height", 0.02F, 1.0F,
                       value.height, warnings);
        if (version >= 2) {
            ReadBoolField(document, kSongRowLayoutSection, prefix + "fluid", value.fluid, warnings);
        }
        ReadIntegerField(document, kSongRowLayoutSection, prefix + "font_size_delta", -8, 16,
                         value.fontSizeDelta, warnings);
        if (const auto text = document.Get(kSongRowLayoutSection, prefix + "font_weight")) {
            if (const auto weight = ParseSongRowWeight(*text)) value.fontWeight = *weight;
            else AddWarning(warnings, "Ignoring invalid song_row_layout." + prefix + "font_weight");
        }
        if (const auto text = document.Get(kSongRowLayoutSection, prefix + "font_style")) {
            if (const auto style = ParseSongRowStyle(*text)) value.fontStyle = *style;
            else AddWarning(warnings, "Ignoring invalid song_row_layout." + prefix + "font_style");
        }
        if (const auto text = document.Get(kSongRowLayoutSection, prefix + "text_color")) {
            if (const auto color = ParseSongRowColor(*text)) value.textColor = *color;
            else AddWarning(warnings, "Ignoring invalid song_row_layout." + prefix + "text_color");
        }
        // Invalid persisted geometry never extends outside the normalized row canvas.
        if ((!value.fluid && value.x + value.width > 1.0F) ||
            value.y + value.height > 1.0F) {
            AddWarning(warnings, "Ignoring out-of-bounds song_row_layout." + prefix + "geometry");
            value = ui::SongRowLayout::Defaults().Field(field);
        }
        value.snap.reset();
        if (version >= 2) {
            if (const auto targetText = document.Get(kSongRowLayoutSection, prefix + "snap_target")) {
                const auto target = ui::SongRowFieldFromKey(*targetText);
                const auto sideText = document.Get(kSongRowLayoutSection, prefix + "snap_side");
                const auto side = sideText ? ParseSongRowSnapSide(*sideText) : std::nullopt;
                int gap = ui::kSongRowDefaultSnapGapPixels;
                bool validGap = false;
                if (const auto gapText = document.Get(kSongRowLayoutSection, prefix + "snap_gap")) {
                    if (const auto parsed = ParseInteger<int>(*gapText)) {
                        gap = *parsed;
                        validGap = gap >= ui::kSongRowMinimumSnapGapPixels &&
                            gap <= ui::kSongRowMaximumSnapGapPixels;
                    }
                }
                if (target && side && validGap) value.snap = {*target, *side, gap};
                else AddWarning(warnings, "Ignoring invalid song_row_layout." + prefix + "snap");
            }
        }
    }
    bool hadInvalidSnap = false;
    for (std::size_t index = 0; index < ui::kSongRowFieldCount; ++index) {
        const auto field = static_cast<ui::SongRowField>(index);
        if (!ui::SongRowSnapIsValid(layout, field)) {
            layout.Field(field).snap.reset();
            hadInvalidSnap = true;
        }
    }
    const bool hasSnapCycle = ui::SongRowHasSnapCycle(layout);
    if (hadInvalidSnap || hasSnapCycle) {
        if (hasSnapCycle) {
            for (auto& field : layout.fields) field.snap.reset();
        }
        AddWarning(warnings, "Ignoring invalid song_row_layout.snap");
    }
}

core::IniDocument MakeSettingsDocument(const AppSettings& settings) {
    core::IniDocument document;
    document.Set("meta", "format", "1");
    // SettingsManager validates musicRoot before serializing, but guard the optional
    // deref so a future caller cannot trigger undefined behavior on an unconvertible path.
    document.Set("library", "music_root", core::EncodeIniValue(
        core::PathToUtf8(settings.musicRoot).value_or(std::string{})));
    document.Set("library", "additional_root_count",
                 std::to_string(settings.additionalMusicRoots.size()));
    for (std::size_t i = 0; i < settings.additionalMusicRoots.size(); ++i) {
        document.Set("library", "additional_root" + std::to_string(i),
                     core::EncodeIniValue(
                         core::PathToUtf8(settings.additionalMusicRoots[i]).value_or(std::string{})));
    }
    document.Set("playback", "volume_percent", std::to_string(settings.volumePercent));
    document.Set("appearance", "skin", settings.skinId);
    WriteSongRowLayout(document, settings.songRowLayout);
    document.Set("appearance", "module_expansion_behavior",
                 settings.moduleExpansionBehavior == ui::ModuleExpansionBehavior::Resize
                     ? "resize" : "squash");
    document.Set("appearance", "window_resize_behavior",
                 settings.windowResizeBehavior == ui::WindowResizeBehavior::GrowTrailingModule
                     ? "grow_trailing_module" : "scale_all");
    document.Set("appearance", "module_resize_behavior",
                 settings.moduleResizeBehavior == ui::ModuleResizeBehavior::Overlap
                     ? "overlap" : "squash");
    document.Set("library", "file_preview_enabled", BoolText(settings.filePreviewEnabled));
    document.Set("library", "preview_fit_window", BoolText(settings.previewFitWindow));
    document.Set("application", "start_at_startup", BoolText(settings.startAtStartup));
    document.Set("application", "exit_to_tray", BoolText(settings.exitToTray));
    document.Set("youtube", "enabled", BoolText(settings.youtubeEnabled));
    document.Set("online", "lyrics_cache_enabled", BoolText(settings.lyricsCacheEnabled));
    document.Set("youtube", "download_kind", std::to_string(settings.youtubeDownloadKind));
    document.Set("youtube", "audio_output_format", std::to_string(settings.youtubeAudioOutputFormat));
    document.Set("youtube", "audio_quality", std::to_string(settings.youtubeAudioQuality));
    document.Set("youtube", "video_height", std::to_string(settings.youtubeVideoHeight));
    document.Set("youtube", "video_fps", std::to_string(settings.youtubeVideoFps));
    document.Set("youtube", "video_extension", settings.youtubeVideoExtension);
    document.Set("youtube", "audio_extension", settings.youtubeAudioExtension);
    document.Set("youtube", "audio_bitrate", std::to_string(settings.youtubeAudioBitrate));
    document.Set("youtube", "grabber_hotkey_modifiers",
                 std::to_string(settings.youtubeGrabberHotkeyModifiers));
    document.Set("youtube", "grabber_hotkey_vk",
                 std::to_string(settings.youtubeGrabberHotkeyVirtualKey));
    document.Set("library", "duplicate_as_file", BoolText(settings.duplicateAsFile));
    document.Set("discord", "enabled", BoolText(settings.discordEnabled));
    document.Set("discord", "show_artist", BoolText(settings.discordShowArtist));
    document.Set("discord", "secondary_text", config::DiscordSecondaryTextText(settings.discordSecondaryText));
    document.Set("discord", "fallback_to_total_streams", BoolText(settings.discordFallbackToTotalStreams));
    document.Set("discord", "show_github_button", BoolText(settings.discordShowGithubButton));
    document.Set("stats", "enabled", BoolText(settings.statsEnabled));
    return document;
}

} // namespace

AppSettings AppSettings::Defaults() {
    AppSettings result;
    result.musicRoot = core::AppPaths::DefaultMusicRoot();
    return result;
}

bool SettingsManager::LoadSettings(std::string* error, std::string* warnings) {
    settings_ = AppSettings::Defaults();
    const auto missing = FileIsMissing(settingsFile_, error);
    if (!missing) return false;
    if (*missing) {
        if (error != nullptr) error->clear();
        return true;
    }

    auto document = core::IniDocument::Load(settingsFile_, error);
    if (!document || !ValidateFormat(*document, "Settings file", error)) return false;

    if (const auto root = document->Get("library", "music_root")) {
        const auto decoded = core::DecodeIniValue(*root);
        const auto path = decoded ? core::PathFromUtf8(*decoded) : std::nullopt;
        if (!path || path->empty()) {
            AddWarning(warnings, "Ignoring invalid library.music_root");
        } else {
            settings_.musicRoot = *path;
        }
    }
    settings_.additionalMusicRoots.clear();
    if (const auto countText = document->Get("library", "additional_root_count")) {
        int count = 0;
        const auto [ptr, ec] = std::from_chars(countText->data(),
                                               countText->data() + countText->size(), count);
        if (ec != std::errc{} || ptr != countText->data() + countText->size() || count < 0) {
            AddWarning(warnings, "Ignoring invalid library.additional_root_count");
        } else {
            constexpr int kMaximumAdditionalRoots = 100;
            if (count > kMaximumAdditionalRoots) {
                AddWarning(warnings, "Limiting library.additional_root_count to " +
                                         std::to_string(kMaximumAdditionalRoots));
                count = kMaximumAdditionalRoots;
            }
            settings_.additionalMusicRoots.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const auto key = "additional_root" + std::to_string(i);
                const auto extra = document->Get("library", key);
                if (!extra) {
                    AddWarning(warnings, "Missing library." + key);
                    continue;
                }
                const auto decoded = core::DecodeIniValue(*extra);
                const auto path = decoded ? core::PathFromUtf8(*decoded) : std::nullopt;
                if (!path || path->empty()) {
                    AddWarning(warnings, "Ignoring invalid library." + key);
                    continue;
                }
                settings_.additionalMusicRoots.push_back(*path);
            }
        }
    } else if (const auto legacy = document->Get("library", "additional_root")) {
        // Pre-list format: single optional additional root.
        const auto decoded = core::DecodeIniValue(*legacy);
        const auto path = decoded ? core::PathFromUtf8(*decoded) : std::nullopt;
        if (decoded && decoded->empty()) {
            // Explicitly unset.
        } else if (!path || path->empty()) {
            AddWarning(warnings, "Ignoring invalid library.additional_root");
        } else {
            settings_.additionalMusicRoots.push_back(*path);
        }
    }
    ReadIntegerField(*document, "playback", "volume_percent", 0, 100,
                     settings_.volumePercent, warnings);
    ReadBoolField(*document, "library", "file_preview_enabled",
                  settings_.filePreviewEnabled, warnings);
    ReadBoolField(*document, "library", "preview_fit_window",
                  settings_.previewFitWindow, warnings);
    ReadBoolField(*document, "library", "duplicate_as_file",
                  settings_.duplicateAsFile, warnings);
    ReadBoolField(*document, "application", "start_at_startup",
                  settings_.startAtStartup, warnings);
    ReadBoolField(*document, "application", "exit_to_tray",
                  settings_.exitToTray, warnings);

    if (const auto skin = document->Get("appearance", "skin")) {
        if (IsIdentifier(*skin)) settings_.skinId = std::string(*skin);
        else AddWarning(warnings, "Ignoring invalid appearance.skin");
    }
    if (HasSongRowLayout(*document)) {
        ReadSongRowLayout(*document, settings_.songRowLayout, warnings);
    } else if (const auto legacyCovers = document->Get("appearance", "track_covers_enabled")) {
        // Pre-builder preference: retain a user's old hidden-cover choice when migrating.
        if (const auto enabled = ParseBool(*legacyCovers)) {
            settings_.songRowLayout.Field(ui::SongRowField::Cover).visible = *enabled;
        } else {
            AddWarning(warnings, "Ignoring invalid appearance.track_covers_enabled");
        }
    }
    if (const auto behavior = document->Get("appearance", "module_expansion_behavior")) {
        if (*behavior == "squash") settings_.moduleExpansionBehavior = ui::ModuleExpansionBehavior::Squash;
        else if (*behavior == "resize") settings_.moduleExpansionBehavior = ui::ModuleExpansionBehavior::Resize;
        else AddWarning(warnings, "Ignoring invalid appearance.module_expansion_behavior");
    }
    if (const auto behavior = document->Get("appearance", "window_resize_behavior")) {
        if (*behavior == "scale_all") settings_.windowResizeBehavior = ui::WindowResizeBehavior::ScaleAll;
        else if (*behavior == "grow_trailing_module") {
            settings_.windowResizeBehavior = ui::WindowResizeBehavior::GrowTrailingModule;
        } else {
            AddWarning(warnings, "Ignoring invalid appearance.window_resize_behavior");
        }
    }
    if (const auto behavior = document->Get("appearance", "module_resize_behavior")) {
        if (*behavior == "squash") settings_.moduleResizeBehavior = ui::ModuleResizeBehavior::Squash;
        else if (*behavior == "overlap") settings_.moduleResizeBehavior = ui::ModuleResizeBehavior::Overlap;
        else AddWarning(warnings, "Ignoring invalid appearance.module_resize_behavior");
    }
    ReadBoolField(*document, "youtube", "enabled", settings_.youtubeEnabled, warnings);
    ReadBoolField(*document, "online", "lyrics_cache_enabled", settings_.lyricsCacheEnabled, warnings);
    ReadIntegerField(*document, "youtube", "download_kind", 0, 1,
                     settings_.youtubeDownloadKind, warnings);
    ReadIntegerField(*document, "youtube", "audio_output_format", 0, 5,
                     settings_.youtubeAudioOutputFormat, warnings);
    ReadIntegerField(*document, "youtube", "audio_quality", 0, 9,
                     settings_.youtubeAudioQuality, warnings);
    ReadIntegerField(*document, "youtube", "video_height", 0, 10000,
                     settings_.youtubeVideoHeight, warnings);
    ReadIntegerField(*document, "youtube", "video_fps", 0, 1000,
                     settings_.youtubeVideoFps, warnings);
    if (const auto value = document->Get("youtube", "video_extension"); value && IsIdentifier(*value)) {
        settings_.youtubeVideoExtension = std::string(*value);
    }
    if (const auto value = document->Get("youtube", "audio_extension"); value && IsIdentifier(*value)) {
        settings_.youtubeAudioExtension = std::string(*value);
    }
    ReadIntegerField(*document, "youtube", "audio_bitrate", 0, 10000,
                     settings_.youtubeAudioBitrate, warnings);
    int grabberHotkeyModifiers = static_cast<int>(settings_.youtubeGrabberHotkeyModifiers);
    ReadIntegerField(*document, "youtube", "grabber_hotkey_modifiers", 0, 0x0fu,
                     grabberHotkeyModifiers, warnings);
    constexpr int allowedGrabberHotkeyModifiers = 0x0001 | 0x0002 | 0x0004 | 0x0008;
    if ((grabberHotkeyModifiers & ~allowedGrabberHotkeyModifiers) != 0) {
        AddWarning(warnings, "Ignoring invalid youtube.grabber_hotkey_modifiers");
    } else {
        settings_.youtubeGrabberHotkeyModifiers =
            static_cast<std::uint32_t>(grabberHotkeyModifiers);
    }
    int grabberHotkeyVirtualKey = static_cast<int>(settings_.youtubeGrabberHotkeyVirtualKey);
    ReadIntegerField(*document, "youtube", "grabber_hotkey_vk", 1, 255,
                     grabberHotkeyVirtualKey, warnings);
    settings_.youtubeGrabberHotkeyVirtualKey =
        static_cast<std::uint32_t>(grabberHotkeyVirtualKey);
    ReadBoolField(*document, "discord", "enabled", settings_.discordEnabled, warnings);
    ReadBoolField(*document, "discord", "show_artist", settings_.discordShowArtist, warnings);
    if (const auto text = document->Get("discord", "secondary_text")) {
        if (const auto parsed = config::ParseDiscordSecondaryText(*text)) {
            settings_.discordSecondaryText = *parsed;
        } else {
            AddWarning(warnings, "Ignoring invalid discord.secondary_text");
        }
    }
    ReadBoolField(*document, "discord", "fallback_to_total_streams",
                  settings_.discordFallbackToTotalStreams, warnings);
    ReadBoolField(*document, "discord", "show_github_button", settings_.discordShowGithubButton, warnings);
    ReadBoolField(*document, "stats", "enabled", settings_.statsEnabled, warnings);

    if (error != nullptr) error->clear();
    return true;
}

bool SettingsManager::SaveSettings(std::string* error) const {
    if (!Validate(settings_, error)) return false;
    return MakeSettingsDocument(settings_).SaveAtomic(settingsFile_, error);
}

bool SettingsManager::SetSettings(AppSettings candidate, std::string* error) {
    if (!Validate(candidate, error)) return false;
    settings_ = std::move(candidate);
    if (error != nullptr) error->clear();
    return true;
}

void SettingsManager::ResetSettings() {
    settings_ = AppSettings::Defaults();
}

bool SettingsManager::Validate(const AppSettings& settings, std::string* error) {
    if (settings.musicRoot.empty() || !core::PathToUtf8(settings.musicRoot)) {
        SetError(error, "Music root must be a non-empty valid Windows path");
        return false;
    }
    for (const auto& root : settings.additionalMusicRoots) {
        if (root.empty() || !core::PathToUtf8(root)) {
            SetError(error, "Additional music folder must be a non-empty valid Windows path");
            return false;
        }
    }
    if (settings.volumePercent < 0 || settings.volumePercent > 100) {
        SetError(error, "Volume must be between 0 and 100");
        return false;
    }
    if (settings.youtubeDownloadKind < 0 || settings.youtubeDownloadKind > 1 ||
        settings.youtubeAudioOutputFormat < 0 || settings.youtubeAudioOutputFormat > 5 ||
        settings.youtubeAudioQuality < 0 || settings.youtubeAudioQuality > 9 ||
        settings.youtubeVideoHeight < 0 || settings.youtubeVideoHeight > 10000 ||
        settings.youtubeVideoFps < 0 || settings.youtubeVideoFps > 1000 ||
        settings.youtubeAudioBitrate < 0 || settings.youtubeAudioBitrate > 10000 ||
        !IsIdentifier(settings.youtubeVideoExtension) ||
        !IsIdentifier(settings.youtubeAudioExtension)) {
        SetError(error, "Invalid YouTube chooser defaults");
        return false;
    }
    constexpr std::uint32_t allowedHotkeyModifiers = 0x0001u | 0x0002u | 0x0004u | 0x0008u;
    if (settings.youtubeGrabberHotkeyVirtualKey == 0 ||
        settings.youtubeGrabberHotkeyVirtualKey > 0xffu ||
        (settings.youtubeGrabberHotkeyModifiers & ~allowedHotkeyModifiers) != 0u) {
        SetError(error, "Invalid YouTube grabber hotkey");
        return false;
    }
    if (!IsIdentifier(settings.skinId)) {
        SetError(error, "Skin identifier must contain 1-64 ASCII letters, digits, '-' or '_'");
        return false;
    }
    if (!std::isfinite(settings.songRowLayout.rowHeight) ||
        settings.songRowLayout.rowHeight < 20.0F || settings.songRowLayout.rowHeight > 160.0F) {
        SetError(error, "Invalid song-row layout dimensions");
        return false;
    }
    for (const auto& field : settings.songRowLayout.fields) {
        if (!std::isfinite(field.x) || !std::isfinite(field.y) ||
            !std::isfinite(field.width) || !std::isfinite(field.height) ||
            field.x < 0.0F || field.x > 1.0F || field.y < 0.0F || field.width < 0.02F ||
            field.width > 1.0F ||
            field.height < 0.02F || (!field.fluid && field.x + field.width > 1.0F) ||
            field.y + field.height > 1.0F || field.fontSizeDelta < -8 ||
            field.fontSizeDelta > 16) {
            SetError(error, "Invalid song-row field layout");
            return false;
        }
        if (field.fontWeight != ui::SongRowFontWeight::Normal &&
            field.fontWeight != ui::SongRowFontWeight::SemiBold &&
            field.fontWeight != ui::SongRowFontWeight::Bold) {
            SetError(error, "Invalid song-row font weight");
            return false;
        }
        if (field.fontStyle != ui::SongRowFontStyle::Normal &&
            field.fontStyle != ui::SongRowFontStyle::Italic) {
            SetError(error, "Invalid song-row font style");
            return false;
        }
        if (field.textColor != ui::SongRowTextColor::Primary &&
            field.textColor != ui::SongRowTextColor::Secondary) {
            SetError(error, "Invalid song-row text color");
            return false;
        }
    }
    for (std::size_t index = 0; index < ui::kSongRowFieldCount; ++index) {
        if (!ui::SongRowSnapIsValid(settings.songRowLayout,
                                    static_cast<ui::SongRowField>(index))) {
            SetError(error, "Invalid song-row field snap");
            return false;
        }
    }
    if (ui::SongRowHasSnapCycle(settings.songRowLayout)) {
        SetError(error, "Song-row field snaps cannot form a cycle");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

} // namespace rivan::config
