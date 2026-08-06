// App.cpp
// Owns Rivan's services and translates window-thread UI actions into queue, audio,
// persistence, scanning, and visualization operations.
#include "App.h"

#include "core/AppPaths.h"
#include "core/IniDocument.h"
#include "core/Text.h"

#include <Windows.h>

#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace rivan {
namespace {

ui::PlaybackState ToUiPlayback(audio::PlaybackState state) noexcept {
    switch (state) {
    case audio::PlaybackState::Playing:
        return ui::PlaybackState::Playing;
    case audio::PlaybackState::Paused:
        return ui::PlaybackState::Paused;
    default:
        return ui::PlaybackState::Stopped;
    }
}

ui::RepeatMode ToUiRepeat(playlist::RepeatMode mode) noexcept {
    switch (mode) {
    case playlist::RepeatMode::All: return ui::RepeatMode::All;
    case playlist::RepeatMode::One: return ui::RepeatMode::One;
    default: return ui::RepeatMode::Off;
    }
}

std::wstring CurrentExecutablePath() {
    std::wstring path(260, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                                static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

} // namespace

App::App(HINSTANCE instance)
    : instance_(instance) {
    youtube_.SetNotify([this]() {
        youtubeDirty_.store(true, std::memory_order_release);
        if (window_ && window_->WindowHandle()) {
            PostMessageW(window_->WindowHandle(), WM_APP + 40, 0, 0);
        }
    });
}

App::~App() {
    youtube_.Reset();
    if (scanThread_.joinable()) {
        scanThread_.request_stop();
        scanThread_.join();
    }
    PersistState();
}

bool App::Initialize() {
    std::wstring pathError;
    if (!core::AppPaths::EnsureDirectories(&pathError)) return false;

    std::string error;
    if (!settings_.Load(&error, nullptr)) {
        settings_.ResetSettings();
        settings_.ResetSession();
    }

    auto applicationSettings = settings_.Settings();
    std::error_code filesystemError;
    if (applicationSettings.musicRoot.empty()) {
        applicationSettings.musicRoot = core::AppPaths::DefaultMusicRoot();
    }
    std::filesystem::create_directories(applicationSettings.musicRoot, filesystemError);
    if (filesystemError || !std::filesystem::is_directory(applicationSettings.musicRoot, filesystemError)) {
        applicationSettings.musicRoot = core::AppPaths::DefaultMusicRoot();
        filesystemError.clear();
        std::filesystem::create_directories(applicationSettings.musicRoot, filesystemError);
        if (filesystemError) return false;
    }
    (void)settings_.SetSettings(applicationSettings, &error);
    std::wstring startupError;
    if (!SyncStartupRegistration(applicationSettings.startAtStartup, &startupError)) {
        applicationSettings.startAtStartup = false;
        (void)settings_.SetSettings(applicationSettings, &error);
        (void)settings_.SaveSettings(&error);
    }

    (void)skins_.Refresh(&error, nullptr);
    committedSkin_ = skins_.Resolve(applicationSettings.skinId);
    activeSkin_ = committedSkin_;

    miniPlayer_ = settings_.Session().miniMode;
    moduleLayout_ = settings_.Session().moduleLayout;
    if (!moduleLayout_.HasValidGeometry()) {
        moduleLayout_ = ui::ModuleLayout::Defaults();
    }
    for (std::size_t i = 0; i < moduleLayout_.items.size(); ++i) {
        auto& item = moduleLayout_.items[i];
        if (moduleLayout_.Find(moduleLayout_.snapGroup[i]) == nullptr) {
            moduleLayout_.snapGroup[i] = item.id;
        }
    }
    queue_.SetShuffle(settings_.Session().shuffle);
    queue_.SetRepeat(ToQueueRepeat(settings_.Session().repeat));
    audio_.SetVolume(static_cast<float>(applicationSettings.volumePercent) / 100.0F);
    audio_.SetEventCallback([this](const audio::AudioEvent& event) {
        if (event.type == audio::AudioEventType::EndOfStream) {
            endOfStream_.store(true, std::memory_order_release);
        }
        audioChanged_.store(true, std::memory_order_release);
    });

    youtube_.RefreshToolStatus();
    youtubeView_ = youtube_.Snapshot();

    discord_.SetEnabled(applicationSettings.discordEnabled);

    window_ = std::make_unique<ui::Win32Ui>(*this);
    ui::WindowOptions options;
    options.initialWidth = miniPlayer_ ? 520 : settings_.Session().window.width;
    options.initialHeight = miniPlayer_ ? 210 : settings_.Session().window.height;
    if (!window_->Create(instance_, options)) return false;

    if (!miniPlayer_) {
        const auto& rectangle = settings_.Session().window;
        SetWindowPos(window_->WindowHandle(), nullptr, rectangle.x, rectangle.y,
                     rectangle.width, rectangle.height, SWP_NOACTIVATE | SWP_NOZORDER);
    }
    StartLibraryScan();
    return true;
}

int App::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        ApplyCompletedScan();
        HandleAudioSignals();
        if (youtubeDirty_.exchange(false, std::memory_order_acq_rel)) {
            OnYoutubeServiceUpdated();
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::SnapshotUiModel(ui::UiModel& out) {
    ApplyCompletedScan();
    HandleAudioSignals();
    if (youtubeDirty_.exchange(false, std::memory_order_acq_rel)) {
        OnYoutubeServiceUpdated();
    }

    // Paint path uses lock-free transport; full Status() only when catalog rebuilds.
    const auto live = audio_.Live();
    // Mini-player has no spectrum pane; skip analysis copy + FFT while hidden there.
    const bool wantVisualization = !miniPlayer_ &&
        live.state == audio::PlaybackState::Playing;
    const auto analysisGeneration = audio_.AnalysisGeneration();
    const auto now = std::chrono::steady_clock::now();
    // UI now presents at display cadence; 30 Hz analyzer input keeps waveform motion
    // fluid without running an FFT for every repaint.
    constexpr auto kMinAnalysisInterval = std::chrono::milliseconds{33};
    if (wantVisualization && analysisGeneration != analysisGeneration_ &&
        (lastAnalysisSubmit_.time_since_epoch().count() == 0 ||
         now - lastAnalysisSubmit_ >= kMinAnalysisInterval)) {
        // Match analyzer FFT size (512 frames of interleaved PCM is enough).
        audio_.AnalysisInto(analysisScratch_, analyzer_.FftSize());
        analyzer_.Submit(analysisScratch_.samples, analysisScratch_.channels,
                         analysisScratch_.sampleRate);
        analysisGeneration_ = analysisScratch_.generation;
        lastAnalysisSubmit_ = now;
    }

    const auto fillLiveFields = [this, &live](ui::UiModel& model) {
        model.positionSeconds = std::chrono::duration<double>(live.position).count();
        model.durationSeconds = std::chrono::duration<double>(live.duration).count();
        model.volume = live.volume;
        model.playback = ToUiPlayback(live.state);
        // Avoid re-copying FFT vectors when the analyzer has not published a new frame.
        analyzer_.CopySnapshot(model.visualization);
        model.revision = revision_;
    };

    // Library/catalog work is expensive; reuse last snapshot when only transport/viz moved.
    if (cachedModelRevision_ == revision_) {
        if (out.revision == revision_) {
            fillLiveFields(out);
            return;
        }
        out = cachedModel_;
        fillLiveFields(out);
        return;
    }

    out.playlists.clear();
    out.tracks.clear();
    out.trackSections.clear();
    out.skins.clear();
    out.nowTitle = L"Nothing playing";
    out.nowArtist.clear();
    out.nowPlayingPath.clear();

    const auto* current = activeTrack_ ? &*activeTrack_ : nullptr;
    const bool trackCoverArtEnabled = settings_.Settings().trackCoverArtEnabled;
    const bool filePreviewEnabled = settings_.Settings().filePreviewEnabled;
    const auto ownerForTrack = [this](library::TrackId trackId,
                                      playlist::PlaylistId fallback) {
        for (const auto& candidate : playlists_.Playlists()) {
            if (candidate.kind != playlist::PlaylistKind::Directory) continue;
            if (std::find(candidate.trackIds.begin(), candidate.trackIds.end(), trackId) !=
                candidate.trackIds.end()) {
                return candidate.id;
            }
        }
        return fallback;
    };
    const auto makeTrackView = [this, current, trackCoverArtEnabled, filePreviewEnabled](
                                   const auto& track, playlist::PlaylistId sourcePlaylistId) {
        ui::TrackView view{track.id, track.title, track.artist, track.album,
                           track.durationSeconds, track.id == selectedTrack_,
                           current != nullptr && current->id == track.id,
                           library::Track::IsAudioFile(track.filePath)};
        // Context-menu rename needs the backing filename even when preview and covers are off.
        view.filePath = track.filePath.wstring();
        view.sourcePlaylistId = sourcePlaylistId;
        return view;
    };

    // Library tree: optional Youtube downloader, flat All Music row, then folders / user.
    if (YoutubeFeatureOn()) {
        const std::size_t ytCount = !youtubeView_.entries.empty()
                                        ? youtubeView_.entries.size()
                                        : 0;
        ui::PlaylistView youtube{playlist::YoutubePlaylistId, L"Youtube", ytCount,
                                 selectedPlaylist_ == playlist::YoutubePlaylistId};
        youtube.depth = 0;
        youtube.youtube = true;
        out.playlists.push_back(std::move(youtube));
    }
    for (const auto& playlist : playlists_.Playlists()) {
        if (playlist.kind != playlist::PlaylistKind::AllMusic) continue;
        ui::PlaylistView view{playlist.id, playlist.name,
                              playlists_.TrackCountRecursive(playlist.id),
                              playlist.id == selectedPlaylist_};
        view.allMusic = true;
        out.playlists.push_back(std::move(view));
    }
    // Depth-first emit of folders, skipping subtrees whose parent is collapsed.
    const std::function<void(playlist::PlaylistId)> emitFolders =
        [&](playlist::PlaylistId parentId) {
            for (const auto* child : playlists_.Children(parentId)) {
                const bool hasChildren = playlists_.HasChildren(child->id);
                const bool expanded = expandedPlaylists_.contains(child->id);
                ui::PlaylistView view{child->id, child->name,
                                      playlists_.TrackCountRecursive(child->id),
                                      child->id == selectedPlaylist_};
                view.depth = child->depth + 1;  // +1 so folders indent under All Music
                view.collapsible = hasChildren;
                view.expanded = hasChildren && expanded;
                // Every scanned folder is an editable filesystem playlist, including roots
                // and nested folders. Destructive actions require UI confirmation.
                view.user = true;
                view.reorderable = true;  // any Directory folder can be reordered among siblings
                view.parentId = child->parentId;
                out.playlists.push_back(std::move(view));
                if (hasChildren && expanded) emitFolders(child->id);
            }
        };
    emitFolders(0);
    for (const auto& playlist : playlists_.Playlists()) {
        if (playlist.kind != playlist::PlaylistKind::User) continue;
        ui::PlaylistView view{playlist.id, playlist.name, playlist.trackIds.size(),
                              playlist.id == selectedPlaylist_};
        view.user = true;
        view.reorderable = true;
        view.parentId = ui::kUserPlaylistGroupParent;
        out.playlists.push_back(std::move(view));
    }

    const bool youtubeBrowsing =
        YoutubeFeatureOn() && selectedPlaylist_ == playlist::YoutubePlaylistId;

    // Current folder / playlist pane. Youtube browser uses youtubeResults; Directory
    // selections use sectioned tracks; other selections use the queue.
    if (youtubeBrowsing) {
        out.youtubeResults.clear();
        const std::size_t page = youtubeView_.searchIsPaged ? youtubeView_.searchPage : 0;
        const std::size_t pageCount =
            youtubeView_.searchIsPaged ? std::max<std::size_t>(1, youtubeView_.searchPageCount)
                                       : 1;
        out.youtubePage = page;
        out.youtubePageCount = pageCount;
        out.youtubeCanPagePrev = youtubeView_.searchIsPaged && page > 0;
        out.youtubeCanPageNext = youtubeView_.searchIsPaged && page + 1 < pageCount;
        const std::size_t begin =
            youtubeView_.searchIsPaged ? page * youtube::kSearchPageSize : 0;
        const std::size_t end =
            youtubeView_.searchIsPaged
                ? std::min(begin + youtube::kSearchPageSize, youtubeView_.entries.size())
                : youtubeView_.entries.size();
        out.youtubeResults.reserve(end > begin ? end - begin : 0);
        for (std::size_t i = begin; i < end; ++i) {
            const auto& entry = youtubeView_.entries[i];
            ui::YoutubeResultView row;
            row.id = entry.id;
            row.title = entry.title;
            row.durationSeconds = entry.durationSeconds;
            row.downloading = entry.downloading;
            row.ready = !entry.localPath.empty();
            row.failed = entry.failed;
            row.selected = entry.id == youtubeSelectedResult_;
            row.downloadProgress = entry.downloadProgress;
            out.youtubeResults.push_back(std::move(row));
        }
    } else if (const auto* selected = playlists_.FindPlaylist(selectedPlaylist_);
               selected != nullptr && selected->kind == playlist::PlaylistKind::Directory) {
        auto directTracks = playlists_.ResolveTracks(selectedPlaylist_);
        if (!directTracks.empty()) {
            const std::size_t first = out.tracks.size();
            for (const auto& track : directTracks) {
                out.tracks.push_back(makeTrackView(track, selectedPlaylist_));
            }
            out.trackSections.push_back({L"", first, directTracks.size()});
        }
        for (const auto* child : playlists_.Children(selectedPlaylist_)) {
            auto childTracks = playlists_.ResolveTracksRecursive(child->id);
            if (childTracks.empty()) continue;
            const std::size_t first = out.tracks.size();
            for (const auto& track : childTracks) {
                out.tracks.push_back(makeTrackView(track, ownerForTrack(track.id, child->id)));
            }
            std::wstring label = child->name;
            std::transform(label.begin(), label.end(), label.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
            out.trackSections.push_back({std::move(label), first, childTracks.size()});
        }
    } else {
        const auto& visibleTracks = queue_.Tracks();
        out.tracks.reserve(visibleTracks.size());
        const auto sourcePlaylistId = selected != nullptr &&
                                      selected->kind == playlist::PlaylistKind::User
                                          ? selectedPlaylist_
                                          : playlist::PlaylistId{};
        for (const auto& track : visibleTracks) {
            out.tracks.push_back(makeTrackView(track, sourcePlaylistId));
        }
    }
    if (current != nullptr) {
        out.nowTitle = current->title;
        out.nowArtist = current->artist.empty() ? current->filePath.parent_path().filename().wstring()
                                                : current->artist;
        if (filePreviewEnabled) out.nowPlayingPath = current->filePath.wstring();
    }
    out.repeat = ToUiRepeat(queue_.Repeat());
    out.shuffle = queue_.Shuffle();
    out.settingsVisible = settingsVisible_;
    out.skinStudioVisible = skinStudioVisible_;
    out.skinStudioEditExisting = skinStudioEditExisting_;
    out.miniPlayer = miniPlayer_;
    out.settingsCategory = settingsCategory_;
    out.musicFolders.clear();
    out.musicFolders.push_back(settings_.Settings().musicRoot.wstring());
    for (const auto& root : settings_.Settings().additionalMusicRoots) {
        out.musicFolders.push_back(root.wstring());
    }
    out.selectedPlaylistId = selectedPlaylist_;
    if (const auto* selectedPlaylist = playlists_.FindPlaylist(selectedPlaylist_)) {
        out.selectedPlaylistIsUser = selectedPlaylist->kind == playlist::PlaylistKind::User ||
            selectedPlaylist->kind == playlist::PlaylistKind::Directory;
        out.selectedPlaylistCanMoveTracks = selectedPlaylist->kind == playlist::PlaylistKind::User;
        out.selectedPlaylistDeletesFiles =
            selectedPlaylist->kind == playlist::PlaylistKind::Directory;
        // Track drag-reorder is allowed on user playlists and every Directory folder.
        out.selectedPlaylistTracksReorderable =
            selectedPlaylist->kind == playlist::PlaylistKind::User ||
            selectedPlaylist->kind == playlist::PlaylistKind::Directory;
        out.selectedPlaylistCanAdd = selectedPlaylist->kind != playlist::PlaylistKind::Youtube;
    } else {
        out.selectedPlaylistIsUser = false;
        out.selectedPlaylistCanMoveTracks = false;
        out.selectedPlaylistDeletesFiles = false;
        out.selectedPlaylistTracksReorderable = false;
        out.selectedPlaylistCanAdd = false;
    }
    out.duplicateAsFile = settings_.Settings().duplicateAsFile;
    out.trackCoverArtEnabled = trackCoverArtEnabled;
    out.filePreviewEnabled = filePreviewEnabled;
    out.startAtStartup = settings_.Settings().startAtStartup;
    out.exitToTray = settings_.Settings().exitToTray;
    out.youtubeEnabled = settings_.Settings().youtubeEnabled;
    out.discordEnabled = settings_.Settings().discordEnabled;
    out.discordShowArtist = settings_.Settings().discordShowArtist;
    out.discordShowImageText = settings_.Settings().discordShowImageText;
    out.discordShowGithubButton = settings_.Settings().discordShowGithubButton;
    out.moduleExpansionBehavior = settings_.Settings().moduleExpansionBehavior;
    out.youtubeMusicSearch = settings_.Settings().youtubeMusicSearch;
    out.youtubeDownloadMode = settings_.Settings().youtubeDownloadMode;
    out.youtubeAudioQuality = settings_.Settings().youtubeAudioQuality;
    out.youtubeMp4VideoQuality = settings_.Settings().youtubeMp4VideoQuality;
    out.youtubeBrowsing =
        YoutubeFeatureOn() && selectedPlaylist_ == playlist::YoutubePlaylistId;
    out.youtubeBusy = youtubeView_.busy;
    out.youtubeYtDlpInstalled = youtubeView_.ytDlpInstalled;
    out.youtubeFfmpegInstalled = youtubeView_.ffmpegInstalled;
    out.youtubeInstallingYtDlp = youtubeView_.installingYtDlp;
    out.youtubeInstallingFfmpeg = youtubeView_.installingFfmpeg;
    out.youtubeStatus = youtubeView_.status;
    out.skins.reserve(skins_.Skins().size());
    for (const auto& skin : skins_.Skins()) {
        out.skins.push_back({core::Utf8ToWide(skin.id, L"Unable to decode error text"),
                             core::Utf8ToWide(skin.name, L"Unable to decode error text"),
                             core::Utf8ToWide(skin.author, L"Unable to decode error text"),
                             skin.builtIn, skin.id == committedSkin_.id});
    }
    out.activeSkin = activeSkin_;
    out.moduleLayout = moduleLayout_;
    out.focusedSkinColor = focusedSkinColor_;
    out.skinColorFocusRevision = skinColorFocusRevision_;
    out.focusedSkinElement = focusedSkinElement_;
    out.skinElementFocusRevision = skinElementFocusRevision_;
    fillLiveFields(out);

    cachedModel_ = out;
    cachedModelRevision_ = revision_;
}

void App::SetModuleLayout(ui::ModuleLayout layout) {
    for (auto& item : layout.items) {
        item.x = std::clamp(item.x, 0.0F, 1.0F);
        item.y = std::clamp(item.y, 0.0F, 1.0F);
        item.width = std::clamp(item.width, item.collapsed ? 0.001F : 0.10F, 1.0F);
        item.height = std::clamp(item.height, item.collapsed ? 0.001F : 0.10F, 1.0F);
        item.x = std::min(item.x, 1.0F - item.width);
        item.y = std::min(item.y, 1.0F - item.height);
        ui::ModuleLayout::SyncExpandedGeometry(item);
    }
    if (layout.tabCount > layout.tabOrder.size()) layout.tabCount = layout.tabOrder.size();
    layout.activeTab = std::min(layout.activeTab, layout.tabCount == 0 ? 0U : layout.tabCount - 1U);
    // Side snapping is represented by geometry and a small explicit state. Keep
    // malformed persisted/host-provided tab entries from referring to an absent
    // module, while retaining the existing global tab-group model.
    for (std::size_t i = 0; i < layout.tabCount; ++i) {
        if (layout.Find(layout.tabOrder[i]) == nullptr) {
            layout.tabCount = 0;
            layout.activeTab = 0;
            break;
        }
    }
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        if (layout.Find(layout.snapGroup[i]) == nullptr) {
            layout.snapGroup[i] = layout.items[i].id;
        }
    }
    moduleLayout_ = layout;
    auto session = settings_.Session();
    session.moduleLayout = moduleLayout_;
    std::string error;
    (void)settings_.SetSession(std::move(session), &error);
    (void)settings_.SaveSession(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::Invoke(ui::Command command) {
    const auto status = audio_.Status();
    switch (command) {
    case ui::Command::PlayPause:
        if (status.state == audio::PlaybackState::Playing) {
            audio_.Pause();
        } else if (status.hasMedia) {
            audio_.Play();
        } else {
            PlayNavigation(queue_.Current() ? playlist::QueueNavigation{
                playlist::QueueNavigationAction::Restarted, queue_.Current()} : queue_.Start());
        }
        break;
    case ui::Command::Stop:
        audio_.Stop();
        break;
    case ui::Command::Previous:
        PlayNavigation(queue_.Previous());
        break;
    case ui::Command::Next:
        PlayNavigation(queue_.Next());
        break;
    case ui::Command::ToggleShuffle:
        queue_.SetShuffle(!queue_.Shuffle());
        break;
    case ui::Command::CycleRepeat:
        switch (queue_.Repeat()) {
        case playlist::RepeatMode::Off: queue_.SetRepeat(playlist::RepeatMode::All); break;
        case playlist::RepeatMode::All: queue_.SetRepeat(playlist::RepeatMode::One); break;
        case playlist::RepeatMode::One: queue_.SetRepeat(playlist::RepeatMode::Off); break;
        }
        break;
    case ui::Command::ToggleSettings:
        settingsVisible_ = !settingsVisible_;
        break;
    case ui::Command::ToggleSkinStudio:
        skinStudioVisible_ = !skinStudioVisible_;
        if (skinStudioVisible_) {
            settingsVisible_ = false;
            // Settings entry point forks a new skin on save; EditSkin sets the flag true.
            skinStudioEditExisting_ = false;
        }
        break;
    case ui::Command::ToggleMiniPlayer:
        ToggleMiniPlayer();
        break;
    case ui::Command::VolumeUp:
        SetVolume(status.volume + 0.05F);
        break;
    case ui::Command::VolumeDown:
        SetVolume(status.volume - 0.05F);
        break;
    case ui::Command::SeekBackward:
        audio_.Seek(std::max(status.position - std::chrono::seconds(5), std::chrono::nanoseconds{}));
        break;
    case ui::Command::SeekForward:
        audio_.Seek(status.duration.count() > 0
                        ? std::min(status.position + std::chrono::seconds(5), status.duration)
                        : status.position + std::chrono::seconds(5));
        break;
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SelectPlaylist(std::uint64_t id) {
    if (id == playlist::YoutubePlaylistId) {
        if (!YoutubeFeatureOn()) return;
        selectedPlaylist_ = id;
        selectedTrack_ = 0;
        youtubeSelectedResult_ = 0;
        if (youtubeView_.entries.empty()) ShowYoutubeLocalLibrary();
        // Prefetch yt-dlp so the first typed search skips cold process load.
        youtube_.Warm();
        ++revision_;
        if (window_) window_->Refresh();
        return;
    }
    if (playlists_.FindPlaylist(id) == nullptr) return;
    selectedPlaylist_ = id;
    selectedTrack_ = 0;
    // Selecting a folder plays everything under it (its loose tracks plus descendants).
    queue_.SetTracks(playlists_.ResolveTracksRecursive(id), std::nullopt);
    // Selecting a folder with subfolders also reveals them, so a single click both
    // opens the folder and shows its playlists in the tree.
    if (playlists_.HasChildren(id)) expandedPlaylists_.insert(id);
    ++revision_;
}

void App::TogglePlaylistExpanded(std::uint64_t id) {
    if (const auto position = expandedPlaylists_.find(id); position != expandedPlaylists_.end()) {
        expandedPlaylists_.erase(position);
    } else {
        expandedPlaylists_.insert(id);
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::RefreshLibrary() {
    restored_ = true;  // Manual rescan must not re-apply saved session selection.
    StartLibraryScan();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::ActivateTrack(std::uint64_t id) {
    auto findInQueue = [this, id]() {
        const auto& tracks = queue_.Tracks();
        return std::find_if(tracks.begin(), tracks.end(), [id](const auto& track) {
            return track.id == id;
        });
    };

    auto found = findInQueue();
    if (found == queue_.Tracks().end()) {
        // Global-library search results may not belong to the selected playlist.
        // Move to All Music so subsequent previous/next navigation remains coherent.
        if (playlists_.FindTrack(id) == nullptr ||
            playlists_.FindPlaylist(playlist::AllMusicPlaylistId) == nullptr) {
            return;
        }
        selectedPlaylist_ = playlist::AllMusicPlaylistId;
        queue_.SetTracks(playlists_.ResolveTracks(selectedPlaylist_), std::nullopt);
        found = findInQueue();
        if (found == queue_.Tracks().end()) return;
    }

    PlayNavigation(queue_.Play(static_cast<std::size_t>(
        std::distance(queue_.Tracks().begin(), found))));
}

void App::Seek(double normalizedPosition) {
    const auto status = audio_.Status();
    if (!status.hasMedia || status.duration.count() <= 0 || !std::isfinite(normalizedPosition)) return;
    normalizedPosition = std::clamp(normalizedPosition, 0.0, 1.0);
    audio_.Seek(std::chrono::nanoseconds(static_cast<std::int64_t>(
        static_cast<double>(status.duration.count()) * normalizedPosition)));
}

void App::SetVolume(float normalizedVolume) {
    if (!std::isfinite(normalizedVolume)) return;
    audio_.SetVolume(std::clamp(normalizedVolume, 0.0F, 1.0F));
}

void App::SelectSettingsCategory(ui::SettingCategory category) {
    settingsCategory_ = category;
    ++revision_;
}

void App::SetMusicFolder(std::size_t index, std::filesystem::path folder) {
    auto settings = settings_.Settings();
    if (index == 0) {
        if (folder.empty()) return;  // Primary root is required.
        settings.musicRoot = std::move(folder);
    } else {
        const std::size_t additionalIndex = index - 1;
        auto& roots = settings.additionalMusicRoots;
        if (folder.empty()) {
            if (additionalIndex >= roots.size()) return;
            roots.erase(roots.begin() + static_cast<std::ptrdiff_t>(additionalIndex));
        } else if (additionalIndex < roots.size()) {
            roots[additionalIndex] = std::move(folder);
        } else if (additionalIndex == roots.size()) {
            roots.push_back(std::move(folder));
        } else {
            return;  // Gap not allowed.
        }
    }
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    RefreshLibrary();  // Rescan with the new folder set.
}

void App::SetFilePreviewEnabled(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.filePreviewEnabled == enabled) return;
    settings.filePreviewEnabled = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetTrackCoverArtEnabled(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.trackCoverArtEnabled == enabled) return;
    settings.trackCoverArtEnabled = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

bool App::SyncStartupRegistration(bool enabled, std::wstring* error) {
    constexpr wchar_t kRunKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kValueName[] = L"Rivan";

    HKEY key{};
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                                   KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        if (error) *error = L"Unable to open the Windows startup registry key.";
        return false;
    }

    bool success = true;
    if (enabled) {
        const std::wstring exePath = CurrentExecutablePath();
        if (exePath.empty()) {
            if (error) *error = L"Unable to resolve the Rivan executable path.";
            success = false;
        } else {
            // Quote the path so it survives spaces when Windows launches it.
            const std::wstring command = L"\"" + exePath + L"\"";
            const auto bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
            status = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(command.c_str()), bytes);
            if (status != ERROR_SUCCESS) {
                if (error) *error = L"Unable to write the Rivan startup entry.";
                success = false;
            }
        }
    } else {
        status = RegDeleteValueW(key, kValueName);
        // Absent value is not an error: the state we want (no autostart) already holds.
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            if (error) *error = L"Unable to remove the Rivan startup entry.";
            success = false;
        }
    }
    RegCloseKey(key);
    return success;
}

void App::SetStartAtStartup(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.startAtStartup == enabled) return;
    // Only persist the toggle if the registry write/removal actually succeeds.
    if (!SyncStartupRegistration(enabled)) return;
    settings.startAtStartup = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetExitToTray(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.exitToTray == enabled) return;
    settings.exitToTray = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetModuleExpansionBehavior(ui::ModuleExpansionBehavior behavior) {
    auto settings = settings_.Settings();
    if (settings.moduleExpansionBehavior == behavior) return;
    settings.moduleExpansionBehavior = behavior;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

namespace {

// Percent-encodes a UTF-8 string so arbitrary paths survive INI key=value lines.
std::string EncodeIni(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const unsigned char ch : value) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                          ch == '.' || ch == '~';
        if (safe) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4U]);
            out.push_back(hex[ch & 0x0FU]);
        }
    }
    return out;
}

std::optional<std::string> DecodeIni(std::string_view value) {
    const auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            out.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) return std::nullopt;
        const int high = hexDigit(value[i + 1]);
        const int low = hexDigit(value[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

std::filesystem::path UserPlaylistsFile() {
    return core::AppPaths::LocalDataRoot() / L"playlists.ini";
}

} // namespace

void App::LoadUserPlaylists() {
    const auto file = UserPlaylistsFile();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    auto document = core::IniDocument::Load(file, nullptr);
    if (!document) return;

    std::size_t count = 0;
    if (const auto stored = document->Get("meta", "count")) {
        const auto text = std::string(*stored);
        std::size_t value = 0;
        const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (err == std::errc{} && end == text.data() + text.size()) count = value;
    }
    if (count > 4096) count = 4096;

    std::vector<playlist::Playlist> users;
    std::vector<library::Track> externalTracks;
    for (std::size_t p = 0; p < count; ++p) {
        const std::string section = "pl" + std::to_string(p);
        const auto nameEncoded = document->Get(section, "name");
        if (!nameEncoded) continue;
        const auto nameUtf8 = DecodeIni(*nameEncoded);
        if (!nameUtf8) continue;
        playlist::Playlist list;
        list.kind = playlist::PlaylistKind::User;
        list.name = core::Utf8ToWide(*nameUtf8);
        std::size_t trackCount = 0;
        if (const auto stored = document->Get(section, "track_count")) {
            const auto text = std::string(*stored);
            std::size_t value = 0;
            const auto [end, err] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (err == std::errc{} && end == text.data() + text.size()) trackCount = value;
        }
        if (trackCount > 100000) trackCount = 100000;
        for (std::size_t t = 0; t < trackCount; ++t) {
            const auto encoded = document->Get(section, "track" + std::to_string(t));
            if (!encoded) continue;
            const auto pathUtf8 = DecodeIni(*encoded);
            if (!pathUtf8) continue;
            const std::filesystem::path path(core::Utf8ToWide(*pathUtf8));
            if (path.empty()) continue;
            auto track = library::Track::FromFile(path);
            list.AppendTrack(track.id);
            externalTracks.push_back(std::move(track));
        }
        users.push_back(std::move(list));
    }
    playlists_.SetUserPlaylists(std::move(users), std::move(externalTracks));
}

void App::SaveUserPlaylists() const {
    core::IniDocument document;
    document.Set("meta", "format", "1");
    const auto users = playlists_.UserPlaylists();
    document.Set("meta", "count", std::to_string(users.size()));
    for (std::size_t p = 0; p < users.size(); ++p) {
        const auto* list = users[p];
        const std::string section = "pl" + std::to_string(p);
        document.Set(section, "name", EncodeIni(core::WideToUtf8(list->name)));
        document.Set(section, "track_count", std::to_string(list->trackIds.size()));
        for (std::size_t t = 0; t < list->trackIds.size(); ++t) {
            const auto* track = playlists_.FindTrack(list->trackIds[t]);
            const std::wstring path = track ? track->filePath.wstring() : std::wstring{};
            document.Set(section, "track" + std::to_string(t),
                         EncodeIni(core::WideToUtf8(path)));
        }
    }
    (void)document.SaveAtomic(UserPlaylistsFile(), nullptr);
}

void App::SaveFolderOrder() const {
    // Dedicated INI in the music root, keyed by folder path so the order maps back onto
    // the same directories after a rescan (ids are recomputed but paths are stable).
    const auto root = settings_.Settings().musicRoot;
    if (root.empty()) return;
    core::IniDocument document;
    document.Set("meta", "format", "1");
    const auto folders = playlists_.FolderOrder();
    document.Set("meta", "count", std::to_string(folders.size()));
    for (std::size_t i = 0; i < folders.size(); ++i) {
        // The depth-first index is the rank; ApplyFolderOrder only compares ranks within a
        // sibling group, so a single monotonic sequence across the whole tree is enough.
        document.Set("fo" + std::to_string(i), "path",
                     EncodeIni(core::WideToUtf8(folders[i]->directory.wstring())));
    }
    (void)document.SaveAtomic(root / L"rivan-folder-order.ini", nullptr);
}

void App::ApplyFolderOrderAfterScan() {
    const auto root = settings_.Settings().musicRoot;
    if (root.empty()) return;
    const auto file = root / L"rivan-folder-order.ini";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    auto document = core::IniDocument::Load(file, nullptr);
    if (!document) return;

    std::size_t count = 0;
    if (const auto stored = document->Get("meta", "count")) {
        const auto text = std::string(*stored);
        std::size_t value = 0;
        const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (err == std::errc{} && end == text.data() + text.size()) count = value;
    }
    if (count > 100000) count = 100000;

    // Map each saved folder path to the Directory playlist id the current scan produced,
    // so ranks apply even though ids are recomputed every scan.
    std::unordered_map<std::wstring, playlist::PlaylistId> idByPath;
    for (const auto& playlist : playlists_.Playlists()) {
        if (playlist.kind == playlist::PlaylistKind::Directory) {
            idByPath.emplace(playlist.directory.wstring(), playlist.id);
        }
    }

    std::unordered_map<playlist::PlaylistId, std::uint32_t> order;
    order.reserve(count);
    std::uint32_t rank = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto encoded = document->Get("fo" + std::to_string(i), "path");
        if (!encoded) continue;
        const auto pathUtf8 = DecodeIni(*encoded);
        if (!pathUtf8) continue;
        const auto found = idByPath.find(core::Utf8ToWide(*pathUtf8));
        if (found == idByPath.end()) continue;  // folder no longer exists
        order.emplace(found->second, rank++);
    }
    playlists_.ApplyFolderOrder(order);
}

void App::SaveTrackOrder(playlist::PlaylistId folderId) const {
    // Track order within Directory folders persists to a dedicated INI keyed by folder
    // path then file path. Only the folder that was just reordered changes, so merge with
    // the existing file to keep other folders' custom orders intact.
    const auto root = settings_.Settings().musicRoot;
    if (root.empty()) return;
    const auto* selected = playlists_.FindPlaylist(folderId);
    if (selected == nullptr || selected->kind != playlist::PlaylistKind::Directory) return;
    const auto file = root / L"rivan-track-order.ini";

    struct Entry {
        std::wstring path;
        std::vector<std::wstring> tracks;
    };
    std::vector<Entry> entries;

    // Carry forward every other folder's saved order.
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && !ec) {
        if (auto document = core::IniDocument::Load(file, nullptr)) {
            std::size_t count = 0;
            if (const auto stored = document->Get("meta", "count")) {
                const auto text = std::string(*stored);
                std::size_t value = 0;
                const auto [end, err] =
                    std::from_chars(text.data(), text.data() + text.size(), value);
                if (err == std::errc{} && end == text.data() + text.size()) count = value;
            }
            if (count > 100000) count = 100000;
            const auto selectedPath = selected->directory.wstring();
            for (std::size_t i = 0; i < count; ++i) {
                const std::string section = "f" + std::to_string(i);
                const auto encPath = document->Get(section, "path");
                if (!encPath) continue;
                const auto pathUtf8 = DecodeIni(*encPath);
                if (!pathUtf8) continue;
                std::wstring folderPath = core::Utf8ToWide(*pathUtf8);
                if (folderPath == selectedPath) continue;  // rewritten below
                std::size_t trackCount = 0;
                if (const auto storedTracks = document->Get(section, "track_count")) {
                    const auto text = std::string(*storedTracks);
                    std::size_t value = 0;
                    const auto [end, err] =
                        std::from_chars(text.data(), text.data() + text.size(), value);
                    if (err == std::errc{} && end == text.data() + text.size()) trackCount = value;
                }
                if (trackCount > 1000000) trackCount = 1000000;
                Entry entry;
                entry.path = std::move(folderPath);
                for (std::size_t j = 0; j < trackCount; ++j) {
                    const auto encTrack = document->Get(section, "track" + std::to_string(j));
                    if (!encTrack) continue;
                    const auto trackUtf8 = DecodeIni(*encTrack);
                    if (!trackUtf8) continue;
                    entry.tracks.push_back(core::Utf8ToWide(*trackUtf8));
                }
                entries.push_back(std::move(entry));
            }
        }
    }

    // Append the selected folder's current order (resolved to stable file paths).
    Entry current;
    current.path = selected->directory.wstring();
    for (const auto id : selected->trackIds) {
        const auto* track = playlists_.FindTrack(id);
        if (track != nullptr) current.tracks.push_back(track->filePath.wstring());
    }
    entries.push_back(std::move(current));

    core::IniDocument document;
    document.Set("meta", "format", "1");
    document.Set("meta", "count", std::to_string(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string section = "f" + std::to_string(i);
        document.Set(section, "path", EncodeIni(core::WideToUtf8(entries[i].path)));
        document.Set(section, "track_count", std::to_string(entries[i].tracks.size()));
        for (std::size_t j = 0; j < entries[i].tracks.size(); ++j) {
            document.Set(section, "track" + std::to_string(j),
                         EncodeIni(core::WideToUtf8(entries[i].tracks[j])));
        }
    }
    (void)document.SaveAtomic(file, nullptr);
}

void App::ApplyTrackOrderAfterScan() {
    const auto root = settings_.Settings().musicRoot;
    if (root.empty()) return;
    const auto file = root / L"rivan-track-order.ini";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    auto document = core::IniDocument::Load(file, nullptr);
    if (!document) return;

    std::size_t count = 0;
    if (const auto stored = document->Get("meta", "count")) {
        const auto text = std::string(*stored);
        std::size_t value = 0;
        const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (err == std::errc{} && end == text.data() + text.size()) count = value;
    }
    if (count > 100000) count = 100000;

    // Folder path -> the Directory playlist this scan produced (ids recomputed, paths stable).
    std::unordered_map<std::wstring, const playlist::Playlist*> folderByPath;
    for (const auto& playlist : playlists_.Playlists()) {
        if (playlist.kind == playlist::PlaylistKind::Directory) {
            folderByPath.emplace(playlist.directory.wstring(), &playlist);
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::string section = "f" + std::to_string(i);
        const auto encPath = document->Get(section, "path");
        if (!encPath) continue;
        const auto pathUtf8 = DecodeIni(*encPath);
        if (!pathUtf8) continue;
        const auto found = folderByPath.find(core::Utf8ToWide(*pathUtf8));
        if (found == folderByPath.end()) continue;  // folder no longer exists
        const auto* folder = found->second;

        // Resolve saved file paths against this folder's current tracks. Matching by path
        // avoids recomputing track-id hashes here; ids are stable but private to the scanner.
        std::unordered_map<std::wstring, library::TrackId> idByFile;
        for (const auto id : folder->trackIds) {
            const auto* track = playlists_.FindTrack(id);
            if (track != nullptr) idByFile.emplace(track->filePath.wstring(), id);
        }

        std::size_t trackCount = 0;
        if (const auto storedTracks = document->Get(section, "track_count")) {
            const auto text = std::string(*storedTracks);
            std::size_t value = 0;
            const auto [end, err] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (err == std::errc{} && end == text.data() + text.size()) trackCount = value;
        }
        if (trackCount > 1000000) trackCount = 1000000;

        std::unordered_map<library::TrackId, std::uint32_t> order;
        std::uint32_t rank = 0;
        for (std::size_t j = 0; j < trackCount; ++j) {
            const auto encTrack = document->Get(section, "track" + std::to_string(j));
            if (!encTrack) continue;
            const auto trackUtf8 = DecodeIni(*encTrack);
            if (!trackUtf8) continue;
            const auto it = idByFile.find(core::Utf8ToWide(*trackUtf8));
            if (it == idByFile.end()) continue;  // track no longer in folder
            order.emplace(it->second, rank++);
        }
        playlists_.ApplyTrackOrder(folder->id, order);
    }
}

} // namespace rivan
