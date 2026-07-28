// Rivan source file
// Purpose: Validated durable preferences and resumable session state.
#include "SettingsManager.h"

#include "../core/AppPaths.h"
#include "../core/IniDocument.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace rivan::config {
namespace {

constexpr std::size_t kMaximumIdentifierBytes = 64;
constexpr std::size_t kMaximumSelectionBytes = 4096;
constexpr std::size_t kMaximumUrlBytes = 2048;
constexpr std::uint64_t kMaximumPositionMilliseconds = 30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

void AddWarning(std::string* warnings, std::string message) {
    if (warnings == nullptr) {
        return;
    }
    if (!warnings->empty()) {
        warnings->push_back('\n');
    }
    *warnings += std::move(message);
}

bool IsValidUtf8(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool IsIdentifier(std::string_view value) noexcept {
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

bool IsHttpUrl(std::string_view value) noexcept {
    if (value.empty()) return true;
    if (value.size() > kMaximumUrlBytes ||
        !(value.starts_with("https://") || value.starts_with("http://"))) {
        return false;
    }
    return IsValidUtf8(value) &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return character > 0x20 && character != 0x7f;
           });
}

std::optional<std::string> PathToUtf8(const std::filesystem::path& path) {
    const std::wstring native = path.native();
    if (native.empty()) {
        return std::string{};
    }
    if (native.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
                                              static_cast<int>(native.size()), nullptr, 0,
                                              nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
                            static_cast<int>(native.size()), result.data(), required,
                            nullptr, nullptr) != required) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::filesystem::path> PathFromUtf8(std::string_view value) {
    if (value.empty() || value.find('\0') != std::string_view::npos ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required) != required) {
        return std::nullopt;
    }
    return std::filesystem::path(result);
}

bool IsUnreserved(unsigned char character) noexcept {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           character == '-' || character == '_' || character == '.' || character == '~';
}

std::string Encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size());
    for (const unsigned char character : value) {
        if (IsUnreserved(character)) {
            output.push_back(static_cast<char>(character));
        } else {
            output.push_back('%');
            output.push_back(hex[character >> 4U]);
            output.push_back(hex[character & 0x0FU]);
        }
    }
    return output;
}

int HexDigit(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

std::optional<std::string> Decode(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            output.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) {
            return std::nullopt;
        }
        const int high = HexDigit(value[index + 1]);
        const int low = HexDigit(value[index + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        output.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    if (!IsValidUtf8(output) || output.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    return output;
}

template <typename Integer>
std::optional<Integer> ParseInteger(std::string_view value) noexcept {
    Integer result{};
    const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (status != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

std::optional<bool> ParseBool(std::string_view value) noexcept {
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    return std::nullopt;
}

std::string BoolText(bool value) {
    return value ? "true" : "false";
}

void ReadIntegerField(const core::IniDocument& document, std::string_view section,
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

void ReadBoolField(const core::IniDocument& document, std::string_view section,
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

void ReadEncodedString(const core::IniDocument& document, std::string_view section,
                       std::string_view key, std::size_t maximumBytes,
                       std::string& destination, std::string* warnings) {
    const auto value = document.Get(section, key);
    if (!value) {
        return;
    }
    const auto decoded = Decode(*value);
    if (!decoded || decoded->size() > maximumBytes) {
        AddWarning(warnings, "Ignoring invalid " + std::string(section) + "." + std::string(key));
        return;
    }
    destination = *decoded;
}

std::optional<bool> FileIsMissing(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        SetError(error, "Unable to inspect " + path.string() + ": " + ec.message());
        return std::nullopt;
    }
    return !exists;
}

bool ValidateFormat(const core::IniDocument& document, std::string_view fileKind,
                    std::string* error) {
    const auto format = document.Get("meta", "format");
    if (!format || *format != "1") {
        SetError(error, std::string(fileKind) + " has an unsupported or missing format");
        return false;
    }
    return true;
}

core::IniDocument MakeSettingsDocument(const AppSettings& settings) {
    core::IniDocument document;
    document.Set("meta", "format", "1");
    // SettingsManager validates musicRoot before serializing, but guard the optional
    // deref so a future caller cannot trigger undefined behavior on an unconvertible path.
    document.Set("library", "music_root", Encode(PathToUtf8(settings.musicRoot).value_or(std::string{})));
    document.Set("library", "additional_root_count",
                 std::to_string(settings.additionalMusicRoots.size()));
    for (std::size_t i = 0; i < settings.additionalMusicRoots.size(); ++i) {
        document.Set("library", "additional_root" + std::to_string(i),
                     Encode(PathToUtf8(settings.additionalMusicRoots[i]).value_or(std::string{})));
    }
    document.Set("playback", "volume_percent", std::to_string(settings.volumePercent));
    document.Set("appearance", "skin", settings.skinId);
    document.Set("appearance", "track_covers_enabled", BoolText(settings.trackCoverArtEnabled));
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
    document.Set("discord", "image_url", settings.discordImageUrl);
    document.Set("discord", "show_artist", BoolText(settings.discordShowArtist));
    document.Set("discord", "show_image_text", BoolText(settings.discordShowImageText));
    return document;
}

core::IniDocument MakeSessionDocument(const SessionState& session) {
    core::IniDocument document;
    document.Set("meta", "format", "1");
    document.Set("window", "x", std::to_string(session.window.x));
    document.Set("window", "y", std::to_string(session.window.y));
    document.Set("window", "width", std::to_string(session.window.width));
    document.Set("window", "height", std::to_string(session.window.height));
    document.Set("window", "mini_mode", BoolText(session.miniMode));
    document.Set("playback", "selected_playlist", Encode(session.selectedPlaylist));
    document.Set("playback", "selected_track", Encode(session.selectedTrack));
    document.Set("playback", "position_ms", std::to_string(session.positionMilliseconds));
    document.Set("playback", "shuffle", BoolText(session.shuffle));
    document.Set("playback", "repeat", std::string(ToString(session.repeat)));
    return document;
}

} // namespace

AppSettings AppSettings::Defaults() {
    AppSettings result;
    result.musicRoot = core::AppPaths::DefaultMusicRoot();
    return result;
}

SessionState SessionState::Defaults() {
    return SessionState{};
}

SettingsManager::SettingsManager()
    : SettingsManager(core::AppPaths::SettingsFile(), core::AppPaths::SessionFile()) {}

SettingsManager::SettingsManager(std::filesystem::path settingsFile,
                                 std::filesystem::path sessionFile)
    : settingsFile_(std::move(settingsFile)),
      sessionFile_(std::move(sessionFile)),
      settings_(AppSettings::Defaults()),
      session_(SessionState::Defaults()) {}

bool SettingsManager::Load(std::string* error, std::string* warnings) {
    if (warnings != nullptr) {
        warnings->clear();
    }
    if (!LoadSettings(error, warnings)) {
        return false;
    }
    return LoadSession(error, warnings);
}

bool SettingsManager::LoadSettings(std::string* error, std::string* warnings) {
    settings_ = AppSettings::Defaults();
    const auto missing = FileIsMissing(settingsFile_, error);
    if (!missing) {
        return false;
    }
    if (*missing) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    auto document = core::IniDocument::Load(settingsFile_, error);
    if (!document || !ValidateFormat(*document, "Settings file", error)) {
        return false;
    }

    if (const auto root = document->Get("library", "music_root")) {
        const auto decoded = Decode(*root);
        const auto path = decoded ? PathFromUtf8(*decoded) : std::nullopt;
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
                const auto decoded = Decode(*extra);
                const auto path = decoded ? PathFromUtf8(*decoded) : std::nullopt;
                if (!path || path->empty()) {
                    AddWarning(warnings, "Ignoring invalid library." + key);
                    continue;
                }
                settings_.additionalMusicRoots.push_back(*path);
            }
        }
    } else if (const auto legacy = document->Get("library", "additional_root")) {
        // Pre-list format: single optional additional root.
        const auto decoded = Decode(*legacy);
        const auto path = decoded ? PathFromUtf8(*decoded) : std::nullopt;
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
        if (IsIdentifier(*skin)) {
            settings_.skinId = std::string(*skin);
        } else {
            AddWarning(warnings, "Ignoring invalid appearance.skin");
        }
    }
    ReadBoolField(*document, "appearance", "track_covers_enabled",
                  settings_.trackCoverArtEnabled, warnings);
    ReadBoolField(*document, "youtube", "enabled", settings_.youtubeEnabled, warnings);
    ReadBoolField(*document, "youtube", "music_search", settings_.youtubeMusicSearch,
                  warnings);
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
    if (const auto imageUrl = document->Get("discord", "image_url")) {
        if (IsHttpUrl(*imageUrl)) {
            settings_.discordImageUrl = std::string(*imageUrl);
        } else {
            AddWarning(warnings, "Ignoring invalid discord.image_url");
        }
    }
    ReadBoolField(*document, "discord", "show_artist",
                  settings_.discordShowArtist, warnings);
    ReadBoolField(*document, "discord", "show_image_text",
                  settings_.discordShowImageText, warnings);

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool SettingsManager::LoadSession(std::string* error, std::string* warnings) {
    session_ = SessionState::Defaults();
    const auto missing = FileIsMissing(sessionFile_, error);
    if (!missing) {
        return false;
    }
    if (*missing) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    auto document = core::IniDocument::Load(sessionFile_, error);
    if (!document || !ValidateFormat(*document, "Session file", error)) {
        return false;
    }

    ReadIntegerField(*document, "window", "x", -32768, 32767, session_.window.x, warnings);
    ReadIntegerField(*document, "window", "y", -32768, 32767, session_.window.y, warnings);
    ReadIntegerField(*document, "window", "width", 320, 16384, session_.window.width, warnings);
    ReadIntegerField(*document, "window", "height", 200, 16384, session_.window.height, warnings);
    ReadBoolField(*document, "window", "mini_mode", session_.miniMode, warnings);
    ReadEncodedString(*document, "playback", "selected_playlist", kMaximumSelectionBytes,
                      session_.selectedPlaylist, warnings);
    ReadEncodedString(*document, "playback", "selected_track", kMaximumSelectionBytes,
                      session_.selectedTrack, warnings);

    if (const auto position = document->Get("playback", "position_ms")) {
        const auto parsed = ParseInteger<std::uint64_t>(*position);
        if (!parsed || *parsed > kMaximumPositionMilliseconds) {
            AddWarning(warnings, "Ignoring invalid playback.position_ms");
        } else {
            session_.positionMilliseconds = *parsed;
        }
    }
    ReadBoolField(*document, "playback", "shuffle", session_.shuffle, warnings);
    if (const auto repeat = document->Get("playback", "repeat")) {
        const auto parsed = ParseRepeatMode(*repeat);
        if (!parsed) {
            AddWarning(warnings, "Ignoring invalid playback.repeat");
        } else {
            session_.repeat = *parsed;
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool SettingsManager::Save(std::string* error) const {
    return SaveSettings(error) && SaveSession(error);
}

bool SettingsManager::SaveSettings(std::string* error) const {
    if (!Validate(settings_, error)) {
        return false;
    }
    return MakeSettingsDocument(settings_).SaveAtomic(settingsFile_, error);
}

bool SettingsManager::SaveSession(std::string* error) const {
    if (!Validate(session_, error)) {
        return false;
    }
    return MakeSessionDocument(session_).SaveAtomic(sessionFile_, error);
}

bool SettingsManager::SetSettings(AppSettings candidate, std::string* error) {
    if (!Validate(candidate, error)) {
        return false;
    }
    settings_ = std::move(candidate);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool SettingsManager::SetSession(SessionState candidate, std::string* error) {
    if (!Validate(candidate, error)) {
        return false;
    }
    session_ = std::move(candidate);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void SettingsManager::ResetSettings() {
    settings_ = AppSettings::Defaults();
}

void SettingsManager::ResetSession() {
    session_ = SessionState::Defaults();
}

bool SettingsManager::Validate(const AppSettings& settings, std::string* error) {
    if (settings.musicRoot.empty() || !PathToUtf8(settings.musicRoot)) {
        SetError(error, "Music root must be a non-empty valid Windows path");
        return false;
    }
    for (const auto& root : settings.additionalMusicRoots) {
        if (root.empty() || !PathToUtf8(root)) {
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
    if (!IsHttpUrl(settings.discordImageUrl)) {
        SetError(error, "Discord image URL must be empty or a valid http(s) URL up to 2048 bytes");
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool SettingsManager::Validate(const SessionState& session, std::string* error) {
    if (session.window.x < -32768 || session.window.x > 32767 ||
        session.window.y < -32768 || session.window.y > 32767 ||
        session.window.width < 320 || session.window.width > 16384 ||
        session.window.height < 200 || session.window.height > 16384) {
        SetError(error, "Window rectangle is outside supported bounds");
        return false;
    }
    if (!IsValidUtf8(session.selectedPlaylist) ||
        session.selectedPlaylist.size() > kMaximumSelectionBytes ||
        session.selectedPlaylist.find('\0') != std::string::npos ||
        !IsValidUtf8(session.selectedTrack) ||
        session.selectedTrack.size() > kMaximumSelectionBytes ||
        session.selectedTrack.find('\0') != std::string::npos) {
        SetError(error, "Playlist and track selections must be valid UTF-8 up to 4096 bytes");
        return false;
    }
    if (session.positionMilliseconds > kMaximumPositionMilliseconds) {
        SetError(error, "Playback position exceeds the 30-day limit");
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::string_view ToString(RepeatMode mode) noexcept {
    switch (mode) {
    case RepeatMode::Off:
        return "off";
    case RepeatMode::All:
        return "all";
    case RepeatMode::One:
        return "one";
    }
    return "off";
}

std::optional<RepeatMode> ParseRepeatMode(std::string_view value) noexcept {
    if (value == "off") {
        return RepeatMode::Off;
    }
    if (value == "all") {
        return RepeatMode::All;
    }
    if (value == "one") {
        return RepeatMode::One;
    }
    return std::nullopt;
}

} // namespace rivan::config
