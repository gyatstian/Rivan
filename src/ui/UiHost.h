// UiHost.h
// Stable value-model and callback seam between the Win32 view and Rivan App.
#pragma once
#include "../skin/Skin.h"
#include "../visualization/Visualization.h"
#include "../youtube/YoutubeService.h"
#include "layout/ModuleLayout.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rivan::ui {

enum class PlaybackState : std::uint8_t { Stopped, Playing, Paused };
enum class RepeatMode : std::uint8_t { Off, All, One };
enum class SettingCategory : std::uint8_t {
    General,
    Appearance,
    Discord,
    Online,
    SkinManager,
};
enum class SkinAssetKind : std::uint8_t { BackgroundImage, Font };

enum class Command : std::uint8_t {
    PlayPause,
    Stop,
    Previous,
    Next,
    ToggleShuffle,
    CycleRepeat,
    ToggleSettings,
    ToggleMiniPlayer,
    VolumeUp,
    VolumeDown,
    SeekBackward,
    SeekForward,
    ToggleSkinStudio,
};

// Sentinel parent id for the User-playlist group. Real Directory folders use their
// manager parentId (scan roots == 0); User playlists share manager parentId 0 but must
// not intermix with root folders during a drag, so the view tags them with this instead.
inline constexpr std::uint64_t kUserPlaylistGroupParent = ~0ULL;

struct PlaylistView {
    std::uint64_t id{};
    std::wstring name;
    std::size_t trackCount{};
    bool selected{};
    // Folder-tree presentation. All Music has depth 0 and is never collapsible.
    std::uint32_t depth{};
    bool collapsible{};  // true when the folder has subfolders
    bool expanded{};     // current expand state (ignored when !collapsible)
    bool allMusic{};     // the special flat All Music row
    bool youtube{};      // the virtual Youtube downloader browser row
    bool user{};         // an editable/renamable/deletable row (root folder or User playlist)
    bool reorderable{};  // can be drag-reordered (any Directory folder or User playlist)
    // Sibling-group key for reorder scoping: Directory folders carry their manager
    // parentId; User playlists carry kUserPlaylistGroupParent.
    std::uint64_t parentId{};
};

// A labeled block of tracks inside the CURRENT FOLDER / PLAYLIST pane. label is empty
// for the selected folder's own loose tracks (shown without a separator header).
struct TrackSection {
    std::wstring label;
    std::size_t first{};  // index into UiModel::tracks
    std::size_t count{};
};

struct TrackView {
    std::uint64_t id{};
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    double durationSeconds{};
    bool selected{};
    bool playing{};
    bool audioFile{};
    // Populated only when file preview or small track covers need it, avoiding path copies
    // while both optional features are disabled.
    std::wstring filePath;
    // Folder/user playlist that owns this entry. Parent-folder views can contain tracks
    // from several descendant folders, so the owner must travel with the visible row.
    std::uint64_t sourcePlaylistId{};
};

// Remote YouTube result or local download shown in the Youtube browser pane.
struct YoutubeResultView {
    std::uint64_t id{};
    std::wstring title;
    double durationSeconds{};
    bool downloading{};
    bool ready{};  // local file available
    bool failed{};
    bool selected{};
    // -1 = n/a; 0..100 while downloading (UI shows percent instead of spinner).
    float downloadProgress{-1.0F};
};

// A summary of an installed/built-in skin for the Skin Manager list.
struct SkinSummary {
    std::wstring id;
    std::wstring name;
    std::wstring author;
    bool builtIn{};
    bool active{};
};

struct UiModel {
    std::vector<PlaylistView> playlists;
    // Tracks shown in the current folder / playlist pane.
    std::vector<TrackView> tracks;
    // Section headers partitioning `tracks` for the current folder view. When empty the
    // whole list renders flat (e.g. All Music).
    std::vector<TrackSection> trackSections;
    std::wstring nowTitle{L"Nothing playing"};
    std::wstring nowArtist;
    // Absolute path of the track currently loaded in the transport. Independent of the
    // browsed playlist so file preview stays on the playing media while navigating.
    std::wstring nowPlayingPath;
    double positionSeconds{};
    double durationSeconds{};
    float volume{0.8F};
    PlaybackState playback{PlaybackState::Stopped};
    RepeatMode repeat{RepeatMode::Off};
    bool shuffle{};
    bool settingsVisible{};
    bool skinStudioVisible{};
    // True when studio opened via Edit on an existing saved skin (save overwrites).
    // False when opened from settings Skin Studio (save creates a new skin).
    bool skinStudioEditExisting{};
    bool miniPlayer{};
    SettingCategory settingsCategory{SettingCategory::General};
    // General pane: library roots in order (index 0 = primary music folder).
    std::vector<std::wstring> musicFolders;
    bool trackCoverArtEnabled{true};
    bool filePreviewEnabled{true};
    bool startAtStartup{};
    bool exitToTray{};
    bool youtubeEnabled{};
    // Discord Rich Presence preference (IPC worker runs only when true).
    bool discordEnabled{};
    bool discordShowArtist{true};
    bool discordShowImageText{true};
    bool discordShowGithubButton{};
    ModuleExpansionBehavior moduleExpansionBehavior{ModuleExpansionBehavior::Squash};
    bool youtubeBrowsing{};  // selected playlist is the virtual Youtube browser
    bool youtubeBusy{};
    bool youtubeYtDlpInstalled{};
    bool youtubeFfmpegInstalled{};
    bool youtubeInstallingYtDlp{};
    bool youtubeInstallingFfmpeg{};
    std::wstring youtubeStatus;
    // Visible page of search results (already sliced in App).
    std::size_t youtubePage{};
    std::size_t youtubePageCount{1};
    bool youtubeCanPagePrev{};
    bool youtubeCanPageNext{};
    std::vector<YoutubeResultView> youtubeResults;
    // Transient state for the separate download-format chooser window.
    bool youtubeChooserVisible{};
    std::uint64_t youtubeChooserEntryId{};
    std::optional<youtube::YoutubeProbe> youtubeProbe;
    youtube::YoutubeDownloadSelection youtubeDownloadSelection;
    std::vector<SkinSummary> skins;
    skin::Skin activeSkin{skin::Skin::BuiltInDarkPurple()};
    visualization::VisualizationSnapshot visualization;
    ModuleLayout moduleLayout{ModuleLayout::Defaults()};
    // Id of the currently selected playlist. User playlists support all edits; library
    // folders and All Music only accept imported tracks.
    std::uint64_t selectedPlaylistId{};
    bool selectedPlaylistIsUser{};
    // Only virtual user playlists support moving references out without touching files.
    bool selectedPlaylistCanMoveTracks{};
    // True when REM deletes the selected source files rather than only playlist entries.
    bool selectedPlaylistDeletesFiles{};
    // True when the selected playlist's tracks can be drag-reordered: user playlists and
    // any Directory folder.
    bool selectedPlaylistTracksReorderable{};
    bool selectedPlaylistCanAdd{};
    // When true, "Duplicate" copies the underlying file on disk; when false it adds a
    // second reference to the same track within the playlist.
    bool duplicateAsFile{};
    std::size_t focusedSkinColor{};
    std::uint64_t skinColorFocusRevision{};
    // Positive values select shape index+1; negative values select -(image index+1).
    int focusedSkinElement{};
    std::uint64_t skinElementFocusRevision{};
    std::uint64_t revision{};
};

class IUiHost {
public:
    virtual ~IUiHost() = default;

    // Every method is called on the window thread. SnapshotUiModel writes owned values
    // into `out` (reusing capacity when the same model is refreshed). App decides state.
    virtual void SnapshotUiModel(UiModel& out) = 0;
    virtual void Invoke(Command command) = 0;
    virtual void SelectPlaylist(std::uint64_t id) = 0;
    // Expand/collapse a folder node in the library tree without changing selection.
    virtual void TogglePlaylistExpanded(std::uint64_t id) = 0;
    // Rescans the configured music folders on demand (manual refresh button).
    virtual void RefreshLibrary() = 0;
    virtual void ActivateTrack(std::uint64_t id) = 0;
    virtual void Seek(double normalizedPosition) = 0;
    virtual void SetVolume(float normalizedVolume) = 0;
    virtual void SelectSettingsCategory(SettingCategory category) = 0;
    // Sets a library folder from the General settings pane. index 0 = primary music
    // root (required; empty ignored). index > 0 = additional root; empty path clears
    // and removes that entry. index may equal current additional count to append.
    // Persists and triggers a rescan.
    virtual void SetMusicFolder(std::size_t index, std::filesystem::path folder) = 0;
    // Off skips all row thumbnail lookup and releases their UI cache.
    virtual void SetTrackCoverArtEnabled(bool enabled) = 0;
    // Off removes preview state and prevents metadata/video work in the view.
    virtual void SetFilePreviewEnabled(bool enabled) = 0;
    virtual void SetStartAtStartup(bool enabled) = 0;
    virtual void SetExitToTray(bool enabled) = 0;
    virtual void SetModuleExpansionBehavior(ModuleExpansionBehavior behavior) = 0;
    // Enables/disables the optional YouTube library section. Off = no YT workers or UI.
    virtual void SetYoutubeEnabled(bool enabled) = 0;
    // Applies the normalized geometry, visibility, and tab state of the main modules.
    virtual void SetModuleLayout(ModuleLayout layout) = 0;
    // Enables/disables Discord Rich Presence. Off = clear activity and stop IPC.
    virtual void SetDiscordEnabled(bool enabled) = 0;
    // Empty restores the built-in Rivan asset. Returns validation failures to the UI.
    virtual void SetDiscordShowArtist(bool enabled) = 0;
    virtual void SetDiscordShowImageText(bool enabled) = 0;
    virtual void SetDiscordShowGithubButton(bool enabled) = 0;
    // One-click install of yt-dlp or ffmpeg into %LOCALAPPDATA%\Rivan\tools.
    virtual void InstallYoutubeTool(bool ytDlp) = 0;
    // Search or resolve a URL when the Youtube playlist is selected.
    virtual void SubmitYoutubeQuery(std::wstring query) = 0;
    // Download (if needed) and play a Youtube result.
    virtual void ActivateYoutubeResult(std::uint64_t id) = 0;
    // Controls the transient format chooser. Hiding through this callback cancels the
    // pending play request; confirmation uses ConfirmYoutubeDownload instead.
    virtual void SetYoutubeChooserVisible(bool visible) = 0;
    virtual void SetYoutubeDownloadKind(youtube::YoutubeDownloadKind kind) = 0;
    virtual void CycleYoutubeVideoFormat(int direction) = 0;
    virtual void CycleYoutubeVideoQuality(int direction) = 0;
    virtual void CycleYoutubeVideoFps(int direction) = 0;
    virtual void CycleYoutubeAudioFormat(int direction) = 0;
    virtual void CycleYoutubeAudioOutput(int direction) = 0;
    virtual void SetYoutubeAudioQuality(int quality) = 0;
    virtual void ConfirmYoutubeDownload() = 0;
    // Client-side search page (0-based).
    virtual void SetYoutubeSearchPage(std::size_t page) = 0;
    // Applies an installed/built-in skin by id and persists the choice.
    virtual void ApplySkin(std::wstring_view id) = 0;
    virtual void EditSkin(std::wstring_view id) = 0;
    [[nodiscard]] virtual bool RenameSkin(std::wstring_view id, std::wstring_view name,
                                          std::wstring& error) = 0;
    [[nodiscard]] virtual bool DeleteSkin(std::wstring_view id, std::wstring& error) = 0;
    virtual void FocusSkinColor(std::size_t index) = 0;
    virtual void FocusSkinElement(int element) = 0;
    // Opens the skins directory in the system file explorer.
    virtual void OpenSkinFolder() = 0;
    virtual void PreviewSkin(skin::Skin candidate) = 0;
    [[nodiscard]] virtual bool SaveSkin(skin::Skin candidate, std::wstring& error) = 0;
    virtual void CancelSkinPreview() = 0;
    [[nodiscard]] virtual std::optional<std::filesystem::path> ImportSkinAsset(
        std::string_view skinId,
        const std::filesystem::path& source,
        SkinAssetKind kind,
        std::wstring& error) = 0;
    virtual void ImportDroppedFiles(std::span<const std::wstring> paths) = 0;

    // ---- Root playlist folders, multi-select editing, drag reordering --------
    // Creates a direct folder under the primary music root and selects it after rescan.
    virtual void CreateUserPlaylist(std::wstring name) = 0;
    // Renames a direct child folder of the primary music root.
    virtual void RenameUserPlaylist(std::uint64_t id, std::wstring name) = 0;
    // Deletes direct child folders of the primary music root and their contents.
    virtual void DeleteUserPlaylists(std::span<const std::uint64_t> ids) = 0;
    // Opens the system file picker and copies files into the selected root playlist folder.
    virtual void AddFilesToSelectedPlaylist() = 0;
    // Removes the entries at the given visible positions from the selected user playlist.
    virtual void RemoveTracksAt(std::span<const std::size_t> indices) = 0;
    // Moves entries at `indices` contiguously to `destination` in playlistId's own list.
    // Both positions refer to the pre-move source list; end == source list size.
    virtual void ReorderSelectedTracks(std::uint64_t playlistId,
                                       std::span<const std::size_t> indices,
                                       std::size_t destination) = 0;
    // Adds the tracks at the given visible positions (of the current view) to another
    // user playlist by id.
    virtual void AddTracksToPlaylist(std::uint64_t targetPlaylistId,
                                     std::span<const std::size_t> indices) = 0;
    // Adds selected entries to target, then removes their source entries from a user playlist.
    virtual void MoveTracksToPlaylist(std::uint64_t targetPlaylistId,
                                      std::span<const std::size_t> indices) = 0;
    // Renames one backing media file and updates any persisted playlist references.
    virtual void RenameTrackAt(std::size_t index, std::wstring name) = 0;
    // Lets Windows' media property handler embed selected image art in audio files.
    virtual void ChangeTracksCover(std::span<const std::size_t> indices) = 0;
    // Duplicates the entries at the given positions in the selected user playlist. When
    // the duplicate-as-file setting is on, copies the underlying files on disk instead of
    // adding a second reference.
    virtual void DuplicateTracksAt(std::span<const std::size_t> indices) = 0;
    // Reorders a folder among its siblings or a virtual user playlist among its peers.
    // `beforeId` 0 means the end of the applicable group.
    virtual void ReorderUserPlaylist(std::uint64_t id, std::uint64_t beforeId) = 0;
    // Moves a folder playlist into another folder playlist. The host changes the backing
    // directory and rescans, so `parentId` must identify a Directory playlist.
    virtual void MovePlaylistInto(std::uint64_t id, std::uint64_t parentId) = 0;
    // Toggles the duplicate-as-file preference.
    virtual void SetDuplicateAsFile(bool enabled) = 0;
};

} // namespace rivan::ui
