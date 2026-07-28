// App.h
// Coordinates Rivan's native services and implements the value-based UI callback seam.
// Startup, queue policy, background scanning, and persisted session ownership live here.
#pragma once

#include "audio/AudioEngine.h"
#include "config/SettingsManager.h"
#include "library/LibraryScanner.h"
#include "playlist/PlaybackQueue.h"
#include "playlist/PlaylistManager.h"
#include "skin/SkinManager.h"
#include "ui/UiHost.h"
#include "ui/Win32Ui.h"
#include "visualization/Visualization.h"
#include "discord/DiscordPresence.h"
#include "youtube/YoutubeService.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_set>

namespace rivan {

class App final : public ui::IUiHost {
public:
    explicit App(HINSTANCE instance);
    ~App() override;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    [[nodiscard]] bool Initialize();
    [[nodiscard]] int Run();

    void SnapshotUiModel(ui::UiModel& out) override;
    void Invoke(ui::Command command) override;
    void SelectPlaylist(std::uint64_t id) override;
    void TogglePlaylistExpanded(std::uint64_t id) override;
    void RefreshLibrary() override;
    void ActivateTrack(std::uint64_t id) override;
    void Seek(double normalizedPosition) override;
    void SetVolume(float normalizedVolume) override;
    void SelectSettingsCategory(ui::SettingCategory category) override;
    void SetMusicFolder(std::size_t index, std::filesystem::path folder) override;
    void SetTrackCoverArtEnabled(bool enabled) override;
    void SetFilePreviewEnabled(bool enabled) override;
    void SetStartAtStartup(bool enabled) override;
    void SetExitToTray(bool enabled) override;
    void SetYoutubeEnabled(bool enabled) override;
    void SetDiscordEnabled(bool enabled) override;
    [[nodiscard]] bool SetDiscordImageUrl(std::wstring url,
                                          std::wstring& error) override;
    void SetDiscordShowArtist(bool enabled) override;
    void SetDiscordShowImageText(bool enabled) override;
    void SetYoutubeMusicSearch(bool enabled) override;
    void SetYoutubeDownloadMode(int mode) override;
    void SetYoutubeAudioQuality(int quality) override;
    void SetYoutubeMp4VideoQuality(int quality) override;
    void InstallYoutubeTool(bool ytDlp) override;
    void SubmitYoutubeQuery(std::wstring query) override;
    void ActivateYoutubeResult(std::uint64_t id) override;
    void SetYoutubeSearchPage(std::size_t page) override;
    void ApplySkin(std::wstring_view id) override;
    void EditSkin(std::wstring_view id) override;
    [[nodiscard]] bool RenameSkin(std::wstring_view id, std::wstring_view name,
                                  std::wstring& error) override;
    [[nodiscard]] bool DeleteSkin(std::wstring_view id, std::wstring& error) override;
    void FocusSkinColor(std::size_t index) override;
    void FocusSkinElement(int element) override;
    void OpenSkinFolder() override;
    void PreviewSkin(skin::Skin candidate) override;
    [[nodiscard]] bool SaveSkin(skin::Skin candidate, std::wstring& error) override;
    void CancelSkinPreview() override;
    [[nodiscard]] std::optional<std::filesystem::path> ImportSkinAsset(
        std::string_view skinId,
        const std::filesystem::path& source,
        ui::SkinAssetKind kind,
        std::wstring& error) override;
    void ImportDroppedFiles(std::span<const std::wstring> paths) override;
    void CreateUserPlaylist(std::wstring name) override;
    void RenameUserPlaylist(std::uint64_t id, std::wstring name) override;
    void DeleteUserPlaylists(std::span<const std::uint64_t> ids) override;
    void AddFilesToSelectedPlaylist() override;
    void RemoveTracksAt(std::span<const std::size_t> indices) override;
    void ReorderSelectedTracks(std::span<const std::size_t> indices,
                               std::size_t destination) override;
    void AddTracksToPlaylist(std::uint64_t targetPlaylistId,
                             std::span<const std::size_t> indices) override;
    void MoveTracksToPlaylist(std::uint64_t targetPlaylistId,
                              std::span<const std::size_t> indices) override;
    void RenameTrackAt(std::size_t index, std::wstring name) override;
    void ChangeTracksCover(std::span<const std::size_t> indices) override;
    void DuplicateTracksAt(std::span<const std::size_t> indices) override;
    void ReorderUserPlaylist(std::uint64_t id, std::uint64_t beforeId) override;
    void MovePlaylistInto(std::uint64_t id, std::uint64_t parentId) override;
    void SetDuplicateAsFile(bool enabled) override;

    // Called from YoutubeService notify (may be worker thread) via PostMessage.
    void OnYoutubeServiceUpdated();

private:
    void StartLibraryScan();
    void ApplyCompletedScan();
    void RestoreSessionAfterScan();
    void HandleAudioSignals();
    void PlayNavigation(const playlist::QueueNavigation& navigation, bool startPlayback = true);
    void UpdateDiscordPresence();
    void PersistState();
    [[nodiscard]] bool SyncStartupRegistration(bool enabled, std::wstring* error = nullptr);
    void ToggleMiniPlayer();
    void ApplyYoutubeSnapshot(bool playIfReady, std::uint64_t playEntryId);
    void ShowYoutubeLocalLibrary();
    // User playlists persist to their own file so they survive restarts and rescans.
    void LoadUserPlaylists();
    void SaveUserPlaylists() const;
    // Custom folder order persists to a dedicated INI in the music root, keyed by folder
    // path, so a drag-reordered tree survives a library rescan.
    void SaveFolderOrder() const;
    void ApplyFolderOrderAfterScan();
    // Custom track order within Directory folders persists to a dedicated INI in the music
    // root, keyed by folder path then file path, so a drag-reordered folder's song order
    // survives a library rescan.
    void SaveTrackOrder() const;
    void ApplyTrackOrderAfterScan();
    // Reseeds the playback queue from the currently selected playlist after an edit.
    void ReseedSelectedUserQueue();
    // Physical file copy for duplicate-as-file; returns the new path or empty on failure.
    [[nodiscard]] std::filesystem::path DuplicateFileOnDisk(
        const std::filesystem::path& source) const;
    // Multi-select audio file picker used by ADD.
    [[nodiscard]] std::vector<std::filesystem::path> PickAudioFiles() const;
    [[nodiscard]] std::filesystem::path PickCoverImage() const;
    // Maps a visible track position in the current view to its library track id.
    [[nodiscard]] std::optional<library::TrackId> TrackIdAtIndex(std::size_t index) const;
    [[nodiscard]] bool YoutubeFeatureOn() const noexcept;
    [[nodiscard]] static playlist::RepeatMode ToQueueRepeat(config::RepeatMode mode) noexcept;
    [[nodiscard]] static config::RepeatMode ToConfigRepeat(playlist::RepeatMode mode) noexcept;

    HINSTANCE instance_{};
    config::SettingsManager settings_;
    skin::SkinManager skins_;
    skin::Skin activeSkin_{skin::Skin::BuiltInDarkPurple()};
    skin::Skin committedSkin_{skin::Skin::BuiltInDarkPurple()};
    playlist::PlaylistManager playlists_;
    playlist::PlaybackQueue queue_;
    // Audio and Youtube workers can notify during their shutdown. These flags must outlive
    // both services, which are destroyed later because members are destroyed in reverse order.
    std::atomic_bool endOfStream_{false};
    std::atomic_bool audioChanged_{false};
    std::atomic_bool youtubeDirty_{false};
    audio::AudioEngine audio_;
    // 512-point FFT is enough for spectrum bars; halves FFT cost vs 1024.
    visualization::FloatSnapshotAnalyzer analyzer_{512};
    youtube::YoutubeService youtube_;
    discord::DiscordPresence discord_;
    std::unique_ptr<ui::Win32Ui> window_;

    std::jthread scanThread_;
    std::mutex scanMutex_;
    std::optional<library::LibraryScanResult> completedScan_;

    playlist::PlaylistId selectedPlaylist_{playlist::AllMusicPlaylistId};
    // Folder tree nodes the user has expanded. Absent = collapsed.
    std::unordered_set<playlist::PlaylistId> expandedPlaylists_;
    library::TrackId selectedTrack_{};
    // Browsing replaces queue contents without interrupting loaded audio.
    std::optional<library::Track> activeTrack_;
    ui::SettingCategory settingsCategory_{ui::SettingCategory::General};
    bool settingsVisible_{};
    bool skinStudioVisible_{};
    bool skinStudioEditExisting_{};
    bool miniPlayer_{};
    std::size_t focusedSkinColor_{};
    std::uint64_t skinColorFocusRevision_{};
    int focusedSkinElement_{};
    std::uint64_t skinElementFocusRevision_{};
    RECT normalWindowRect_{};
    std::uint64_t revision_{};
    std::uint64_t analysisGeneration_{};
    // Reused across paints so AnalysisInto does not allocate every generation.
    audio::AudioAnalysisSnapshot analysisScratch_{};
    // Cap FFT to ~20 Hz even if analysis generation advances every WASAPI period.
    std::chrono::steady_clock::time_point lastAnalysisSubmit_{};
    // Last fully built library/UI snapshot. Live playback fields refresh in place.
    ui::UiModel cachedModel_{};
    std::uint64_t cachedModelRevision_{~std::uint64_t{0}};
    bool restored_{};

    // Youtube browser state mirrored for UI (updated on worker notify).
    youtube::YoutubeSnapshot youtubeView_{};
    std::uint64_t youtubeSelectedResult_{};
    std::uint64_t pendingPlayYoutubeId_{};
};

} // namespace rivan
