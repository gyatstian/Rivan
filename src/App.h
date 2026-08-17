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
#include "lyrics/LyricsService.h"
#include "stats/StatsService.h"
#include "update/UpdateService.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_set>
#include <utility>

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
    void OnMainWindowClosing() override;
    void OnMainWindowClosingToTray() override;
    void Invoke(ui::Command command) override;
    void SelectPlaylist(std::uint64_t id) override;
    void TogglePlaylistExpanded(std::uint64_t id) override;
    void RefreshLibrary() override;
    void ActivateTrack(std::uint64_t id) override;
    void Seek(double normalizedPosition) override;
    void SetVolume(float normalizedVolume) override;
    void SelectSettingsCategory(ui::SettingCategory category) override;
    void SetMusicFolder(std::size_t index, std::filesystem::path folder) override;
    void SetSongRowLayout(ui::SongRowLayout layout) override;
    void PreviewSongRowLayout(ui::SongRowLayout layout) override;
    void SetFilePreviewEnabled(bool enabled) override;
    void SetPreviewFitWindow(bool enabled) override;
    void SetStartAtStartup(bool enabled) override;
    void SetExitToTray(bool enabled) override;
    void SetModuleExpansionBehavior(ui::ModuleExpansionBehavior behavior) override;
    void SetWindowResizeBehavior(ui::WindowResizeBehavior behavior) override;
    void SetModuleResizeBehavior(ui::ModuleResizeBehavior behavior) override;
    void SetYoutubeEnabled(bool enabled) override;
    void GrabYoutubeLink(std::wstring url) override;
    [[nodiscard]] bool SetYoutubeGrabberHotkey(std::uint32_t modifiers,
                                               std::uint32_t virtualKey) override;
    void SetLyricsCacheEnabled(bool enabled) override;
    void SetLyricsOnlineEnabled(bool enabled) override;
    void SetLyricsFakeTimestampsEnabled(bool enabled) override;
    void SetTrackLyricsDisabled(std::wstring filePath, bool disabled) override;
    void AddLyricsToTrack(std::wstring title, std::wstring artist, std::wstring album,
                          double durationSeconds, std::wstring filePath) override;
    void SetLyricsAlignment(config::LyricsTextAlignment alignment) override;
    void AddYourOwnLyrics() override;
    void SetStatsEnabled(bool enabled) override;
    void SetStatisticsPeriod(stats::DashboardPeriod period) override;
    void SetStatisticsTracksExpanded(bool expanded) override;
    void SetStatisticsArtistsExpanded(bool expanded) override;
    void SetStatisticsTracksPage(std::size_t page) override;
    void SetStatisticsArtistsPage(std::size_t page) override;
    void SetModuleLayout(ui::ModuleLayout layout) override;
    void SetDiscordEnabled(bool enabled) override;
    void SetDiscordShowArtist(bool enabled) override;
    void SetDiscordSecondaryText(config::DiscordSecondaryText mode) override;
    void SetDiscordFallbackToTotalStreams(bool enabled) override;
    void SetDiscordShowGithubButton(bool enabled) override;
    void InstallYoutubeTool(bool ytDlp) override;
    void SubmitYoutubeQuery(std::wstring query) override;
    void ActivateYoutubeResult(std::uint64_t id) override;
    void SetYoutubeChooserVisible(bool visible) override;
    void SetUpdateNotifierVisible(bool visible) override;
    void OpenUpdateRelease() override;
    void SetYoutubeDownloadKind(youtube::YoutubeDownloadKind kind) override;
    void CycleYoutubeVideoFormat(int direction) override;
    void CycleYoutubeVideoQuality(int direction) override;
    void CycleYoutubeVideoFps(int direction) override;
    void CycleYoutubeAudioFormat(int direction) override;
    void CycleYoutubeAudioOutput(int direction) override;
    void SetYoutubeAudioQuality(int quality) override;
    void ConfirmYoutubeDownload() override;
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
    void ReorderSelectedTracks(std::uint64_t playlistId,
                               std::span<const std::size_t> indices,
                               std::size_t destination) override;
    void AddTracksToPlaylist(std::uint64_t targetPlaylistId,
                             std::span<const std::size_t> indices) override;
    void MoveTracksToPlaylist(std::uint64_t targetPlaylistId,
                              std::span<const std::size_t> indices) override;
    void RenameTrackAt(std::size_t index, std::wstring name) override;
    void ChangeTracksCover(std::span<const std::size_t> indices) override;
    void ChangeTrackMetadata(std::span<const std::size_t> indices,
                             ui::TrackMetadataField field,
                             const std::wstring& value) override;
    void DuplicateTracksAt(std::span<const std::size_t> indices) override;
    void ReorderUserPlaylist(std::uint64_t id, std::uint64_t beforeId) override;
    void MovePlaylistInto(std::uint64_t id, std::uint64_t parentId) override;
    void SetDuplicateAsFile(bool enabled) override;

    // Called from YoutubeService notify (may be worker thread) via PostMessage.
    void OnYoutubeServiceUpdated();
    void OnLyricsServiceUpdated();
    // Called from UpdateService notify (may be worker thread) via PostMessage.
    void OnUpdateServiceUpdated();

private:
    void StartLibraryScan();
    // Root the session actually runs on: musicRootOverride_ when set (startup fallback),
    // else the configured settings_.Settings().musicRoot. Never persisted; the configured
    // root always stays in settings_ and on disk.
    [[nodiscard]] std::filesystem::path EffectiveMusicRoot() const noexcept;
    void ApplyCompletedScan();
    void RestoreSessionAfterScan();
    void HandleAudioSignals();
    void NotifyAudioSignal();
    // Waits until the audio worker finishes the placeholder Load ("__RIVAN_EDIT__")
    // and reports the release (Error, no media), or a bounded wait elapses.
    // Returns true only when the release was observed.
    [[nodiscard]] bool WaitForAudioRelease() const;
    void PlayNavigation(const playlist::QueueNavigation& navigation, bool startPlayback = true);
    void UpdateDiscordPresence();
    void PersistState();
    // Copies the current selectedPlaylist_/selectedTrack_ into the in-memory session
    // so that any SaveSession (SetModuleLayout, Initialize, PersistState) writes the
    // up-to-date selection. Called from PlayNavigation, SelectPlaylist, and the restore.
    void SyncPlaybackSession();
    [[nodiscard]] bool SyncStartupRegistration(bool enabled, std::wstring* error = nullptr);
    void ToggleMiniPlayer();
    void ApplyYoutubeSnapshot();
    void SetUpdateNotifierVisibleInternal(bool visible);
    void PersistYoutubeChooserSelection();
    void ShowYoutubeLocalLibrary();
    // User playlists persist to their own file so they survive restarts and rescans.
    void LoadUserPlaylists();
    void SaveUserPlaylists() const;
    // Per-song "lyrics disabled" choices persist to their own file, keyed by song path.
    void LoadDisabledLyrics();
    void SaveDisabledLyrics() const;
    // Custom folder order persists to a dedicated INI in the music root, keyed by folder
    // path, so a drag-reordered tree survives a library rescan.
    void SaveFolderOrder() const;
    void ApplyFolderOrderAfterScan();
    // Custom track order within Directory folders persists to a dedicated INI in the music
    // root, keyed by folder path then file path, so a drag-reordered folder's song order
    // survives a library rescan.
    void SaveTrackOrder(playlist::PlaylistId folderId) const;
    void ApplyTrackOrderAfterScan();
    // Reseeds the playback queue from the currently selected playlist after an edit.
    void ReseedSelectedUserQueue();
    // Physical file copy for duplicate-as-file; returns the new path or empty on failure.
    [[nodiscard]] std::filesystem::path DuplicateFileOnDisk(
        const std::filesystem::path& source) const;
    // Multi-select audio file picker used by ADD.
    [[nodiscard]] std::vector<std::filesystem::path> PickAudioFiles() const;
    [[nodiscard]] std::filesystem::path PickCoverImage() const;
    // Fallback title/artist derivation shared by lyrics requests and the custom lyrics
    // file naming (defined in AppLibrary.cpp).
    [[nodiscard]] static std::pair<std::wstring, std::wstring> LyricsMetadata(
        const library::Track& track);
    // Re-requests lyrics for the active track so preference toggles (online-only, fake
    // timestamps) apply immediately instead of waiting for the next song.
    void RefreshActiveLyrics();
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
    // Audio emits this at a bounded rate while playing. It keeps lyric presence
    // updates alive when rendering is paused (minimized/hidden window).
    std::atomic_bool audioPositionChanged_{false};
    std::atomic_bool discordTimestampRefreshRequested_{false};
    std::atomic_bool discordPositionUpdatesEnabled_{false};
    // Set from the audio callback after a navigation request. A Live() snapshot can
    // still describe the old track as Playing until the queued Load begins.
    std::atomic_bool discordTrackTransitionLoadingObserved_{false};
    std::atomic<HWND> audioNotificationWindow_{nullptr};
    std::atomic_bool youtubeDirty_{false};
    // True while a library scan is in flight or its result has not been applied yet.
    bool scanRunning_{};
    // Session-only override used when the configured music root is temporarily
    // unavailable (e.g. removable drive unplugged). The configured root stays in
    // settings_ and is never overwritten on disk. Empty = use configured root.
    std::filesystem::path musicRootOverride_;
    // A download finished while a scan was in flight; rescan once it has applied.
    bool downloadRescanPending_{};
    std::atomic_bool lyricsDirty_{false};
    std::atomic_bool updateDirty_{false};
    std::atomic<HWND> updateNotificationWindow_{nullptr};
    DWORD uiThreadId_{};
    audio::AudioEngine audio_;
    // Constructed after audio_ (transport source) and destroyed before it.
    stats::StatsService stats_;
    // 512-point FFT is enough for spectrum bars; halves FFT cost vs 1024.
    visualization::FloatSnapshotAnalyzer analyzer_{512};
    youtube::YoutubeService youtube_;
    lyrics::LyricsService lyrics_;
    update::UpdateService update_;
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
    stats::DashboardPeriod statisticsPeriod_{stats::DashboardPeriod::Week};
    bool statisticsTracksExpanded_{};
    bool statisticsArtistsExpanded_{};
    std::size_t statisticsTracksPage_{};
    std::size_t statisticsArtistsPage_{};
    std::uint64_t analysisGeneration_{};
    // Reused across paints so AnalysisInto does not allocate every generation.
    audio::AudioAnalysisSnapshot analysisScratch_{};
    // Lyrics snapshot is copied only when the service revision advances.
    std::uint64_t lyricsRevisionCache_{~std::uint64_t{0}};
    lyrics::LyricsSnapshot lyricsSnapshotCache_;
    // Song file paths (normalized) whose lyrics the user disabled; persisted.
    std::unordered_set<std::wstring> disabledLyricsSongs_;
    // Last published Discord presence surface. Timestamps stay outside these keys so
    // 30 Hz playback paints do not send Discord RPC updates.
    bool discordPresencePublished_{};
    std::string discordPresenceStateKey_;
    std::string discordPresenceBaseKey_;
    library::TrackId discordPresenceTrackId_{};
    // Keeps the transient audio Load state from clearing the new track presence.
    bool discordTrackTransitionPending_{};
    bool discordTrackTransitionPublished_{};
    // A seek requests one timestamp correction; ordinary position notifications do not.
    bool discordTimestampRefreshPending_{};
    // Cap FFT to ~20 Hz even if analysis generation advances every WASAPI period.
    std::chrono::steady_clock::time_point lastAnalysisSubmit_{};
    // Last fully built library/UI snapshot. Live playback fields refresh in place.
    ui::UiModel cachedModel_{};
    std::uint64_t cachedModelRevision_{~std::uint64_t{0}};
    std::shared_ptr<const stats::ListenStatsModel> statisticsSnapshotCache_;
    stats::DashboardCatalog statisticsCatalogCache_;
    std::uint64_t statisticsCatalogCacheRevision_{~std::uint64_t{0}};
    stats::DashboardData statisticsDashboardCache_{};
    std::uint64_t statisticsDashboardCatalogRevision_{~std::uint64_t{0}};
    std::uint64_t statisticsDashboardGeneration_{};
    bool restored_{};
    // User playlists restore once, on the first scan application, so the per-track
    // metadata reads do not delay the initial scan start.
    bool userPlaylistsLoaded_{};
    bool moduleLayoutWarning_{};

    // Youtube browser state mirrored for UI (updated on worker notify).
    youtube::YoutubeSnapshot youtubeView_{};
    std::uint64_t youtubeSelectedResult_{};
    bool youtubeChooserVisible_{};
    std::uint64_t youtubeChooserEntryId_{};
    youtube::YoutubeDownloadSelection youtubeDownloadSelection_{};
    bool pendingYoutubeGrab_{};
    bool youtubeGrabberHotkeyAvailable_{true};
    std::shared_ptr<const update::UpdateSnapshot> updateSnapshot_;
    bool updateNotifierVisible_{};
    ui::ModuleLayout moduleLayout_{ui::ModuleLayout::Defaults()};
    // Throttle layout INI writes during interactive resize; session stays in memory.
    std::chrono::steady_clock::time_point lastSessionSave_{};
    // Throttle Youtube chooser preference writes; settings stay in memory every click.
    std::chrono::steady_clock::time_point lastYoutubeChooserSave_{};
    // PersistState runs on WM_CLOSE and again in ~App; skip the destructor duplicate.
    bool persistedOnClose_{};
};

} // namespace rivan
