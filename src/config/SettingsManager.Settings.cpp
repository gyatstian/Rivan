// SettingsManager.Settings.cpp
// Durable application-preference serialization and validation.
#include "SettingsManager.Persistence.h"

#include "../core/AppPaths.h"

namespace rivan::config {
namespace {

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
    document.Set("appearance", "track_covers_enabled", BoolText(settings.trackCoverArtEnabled));
    document.Set("appearance", "module_expansion_behavior",
                 settings.moduleExpansionBehavior == ui::ModuleExpansionBehavior::Resize
                     ? "resize" : "squash");
    document.Set("appearance", "window_resize_behavior",
                 settings.windowResizeBehavior == ui::WindowResizeBehavior::GrowTrailingModule
                     ? "grow_trailing_module" : "scale_all");
    document.Set("library", "file_preview_enabled", BoolText(settings.filePreviewEnabled));
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
    document.Set("discord", "show_image_text", BoolText(settings.discordShowImageText));
    document.Set("discord", "show_github_button", BoolText(settings.discordShowGithubButton));
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
    ReadBoolField(*document, "appearance", "track_covers_enabled",
                  settings_.trackCoverArtEnabled, warnings);
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
    ReadBoolField(*document, "discord", "show_image_text", settings_.discordShowImageText, warnings);
    ReadBoolField(*document, "discord", "show_github_button", settings_.discordShowGithubButton, warnings);

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
    if (error != nullptr) error->clear();
    return true;
}

} // namespace rivan::config
