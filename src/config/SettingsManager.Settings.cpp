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
    document.Set("library", "file_preview_enabled", BoolText(settings.filePreviewEnabled));
    document.Set("application", "start_at_startup", BoolText(settings.startAtStartup));
    document.Set("application", "exit_to_tray", BoolText(settings.exitToTray));
    document.Set("youtube", "enabled", BoolText(settings.youtubeEnabled));
    document.Set("youtube", "music_search", BoolText(settings.youtubeMusicSearch));
    document.Set("youtube", "download_mode", std::to_string(settings.youtubeDownloadMode));
    document.Set("youtube", "audio_quality", std::to_string(settings.youtubeAudioQuality));
    document.Set("youtube", "mp4_video_quality",
                 std::to_string(settings.youtubeMp4VideoQuality));
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
    ReadBoolField(*document, "youtube", "enabled", settings_.youtubeEnabled, warnings);
    ReadBoolField(*document, "youtube", "music_search", settings_.youtubeMusicSearch, warnings);
    if (document->Get("youtube", "download_mode")) {
        ReadIntegerField(*document, "youtube", "download_mode", 0, 2,
                         settings_.youtubeDownloadMode, warnings);
    } else {
        // Legacy migration: old convert_to_mp3 bool. true -> MP3 (0), false -> Video (2).
        bool legacyConvert = true;
        ReadBoolField(*document, "youtube", "convert_to_mp3", legacyConvert, warnings);
        settings_.youtubeDownloadMode = legacyConvert ? 0 : 2;
    }
    ReadIntegerField(*document, "youtube", "audio_quality", 0, 9,
                     settings_.youtubeAudioQuality, warnings);
    ReadIntegerField(*document, "youtube", "mp4_video_quality", 0, 5,
                     settings_.youtubeMp4VideoQuality, warnings);
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
    if (settings.youtubeAudioQuality < 0 || settings.youtubeAudioQuality > 9) {
        SetError(error, "YouTube audio quality must be between 0 and 9");
        return false;
    }
    if (settings.youtubeMp4VideoQuality < 0 || settings.youtubeMp4VideoQuality > 5) {
        SetError(error, "YouTube MP4 video quality must be between 0 and 5");
        return false;
    }
    if (settings.youtubeDownloadMode < 0 || settings.youtubeDownloadMode > 2) {
        SetError(error, "YouTube download mode must be between 0 and 2");
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
