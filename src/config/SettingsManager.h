// Rivan source file
// Purpose: Validated durable preferences and resumable session state.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "../ui/layout/ModuleLayout.h"

namespace rivan::config {

struct WindowRect final {
    int x = 100;
    int y = 100;
    int width = 1100;
    int height = 720;
};

enum class RepeatMode {
    Off,
    All,
    One,
};

struct AppSettings final {
    std::filesystem::path musicRoot;
    // Extra library roots (any count). Subfolders of every root become playlists.
    std::vector<std::filesystem::path> additionalMusicRoots;
    int volumePercent = 80;
    std::string skinId = "dark-purple";
    // Optional small cover thumbnails beside titles in library and playlist rows.
    bool trackCoverArtEnabled = true;
    // Optional current-folder media preview. Off means no preview metadata or media work.
    bool filePreviewEnabled = true;
    // Registers Rivan for the current user's Windows sign-in.
    bool startAtStartup = false;
    // Closing the main window keeps Rivan running behind a notification-area icon.
    bool exitToTray = false;
    // Optional YouTube browse/download (yt-dlp). Off by default; no YT work when false.
    bool youtubeEnabled = false;
    // YouTube chooser defaults. These are not exposed in Preferences; chooser updates them.
    int youtubeDownloadKind = 1;       // 0 = video + audio, 1 = audio only
    int youtubeAudioOutputFormat = 0;  // 0 = native, 1 = mp3, 2 = aac, 3 = opus, 4 = flac, 5 = wav
    int youtubeAudioQuality = 0;       // 0 = best .. 9 = smallest
    int youtubeVideoHeight = 0;
    int youtubeVideoFps = 0;
    std::string youtubeVideoExtension = "mp4";
    std::string youtubeAudioExtension = "m4a";
    int youtubeAudioBitrate = 0;
    // Right-click "Duplicate": false = add a second reference to the same track within
    // the playlist (no file copied); true = copy the underlying file on disk and add it.
    bool duplicateAsFile = false;
    // Optional Discord Rich Presence (IPC). Off by default; no Discord I/O when false.
    bool discordEnabled = false;
    // Optional Rich Presence fields. Discord's application label itself is not removable.
    bool discordShowArtist = true;
    bool discordShowImageText = true;
    bool discordShowGithubButton = false;
    ui::ModuleExpansionBehavior moduleExpansionBehavior =
        ui::ModuleExpansionBehavior::Squash;

    [[nodiscard]] static AppSettings Defaults();
};

struct SessionState final {
    WindowRect window;
    bool miniMode = false;
    std::string selectedPlaylist;
    std::string selectedTrack;
    std::uint64_t positionMilliseconds = 0;
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::Off;
    ui::ModuleLayout moduleLayout{ui::ModuleLayout::Defaults()};

    [[nodiscard]] static SessionState Defaults();
};

class SettingsManager final {
public:
    SettingsManager();
    SettingsManager(std::filesystem::path settingsFile, std::filesystem::path sessionFile);

    // Missing files are not errors. Invalid persisted fields use their defaults and
    // are described in warnings, separated by newlines.
    [[nodiscard]] bool Load(std::string* error = nullptr, std::string* warnings = nullptr);
    [[nodiscard]] bool LoadSettings(std::string* error = nullptr, std::string* warnings = nullptr);
    [[nodiscard]] bool LoadSession(std::string* error = nullptr, std::string* warnings = nullptr);

    [[nodiscard]] bool Save(std::string* error = nullptr) const;
    [[nodiscard]] bool SaveSettings(std::string* error = nullptr) const;
    [[nodiscard]] bool SaveSession(std::string* error = nullptr) const;

    // Candidates are rejected as a whole if any field is invalid.
    [[nodiscard]] bool SetSettings(AppSettings candidate, std::string* error = nullptr);
    [[nodiscard]] bool SetSession(SessionState candidate, std::string* error = nullptr);

    [[nodiscard]] const AppSettings& Settings() const noexcept { return settings_; }
    [[nodiscard]] const SessionState& Session() const noexcept { return session_; }
    [[nodiscard]] const std::filesystem::path& SettingsPath() const noexcept { return settingsFile_; }
    [[nodiscard]] const std::filesystem::path& SessionPath() const noexcept { return sessionFile_; }

    void ResetSettings();
    void ResetSession();

    [[nodiscard]] static bool Validate(const AppSettings& settings, std::string* error = nullptr);
    [[nodiscard]] static bool Validate(const SessionState& session, std::string* error = nullptr);

private:
    std::filesystem::path settingsFile_;
    std::filesystem::path sessionFile_;
    AppSettings settings_;
    SessionState session_;
};

[[nodiscard]] std::string_view ToString(RepeatMode mode) noexcept;
[[nodiscard]] std::optional<RepeatMode> ParseRepeatMode(std::string_view value) noexcept;

} // namespace rivan::config
