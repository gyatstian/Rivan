// AppUiProjection.cpp
// Builds the value-based UI model from application-owned service state.
#include "App.h"

#include "core/Text.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <functional>
#include <unordered_map>
#include <utility>

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

} // namespace

void App::SnapshotUiModel(ui::UiModel& out) {
    ApplyCompletedScan();
    HandleAudioSignals();
    if (youtubeDirty_.exchange(false, std::memory_order_acq_rel)) {
        OnYoutubeServiceUpdated();
    }
    if (lyricsDirty_.exchange(false, std::memory_order_acq_rel)) {
        OnLyricsServiceUpdated();
    }
    if (updateDirty_.exchange(false, std::memory_order_acq_rel)) {
        OnUpdateServiceUpdated();
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
        const auto lyricsRevision = lyrics_.Revision();
        if (lyricsRevision != lyricsRevisionCache_) {
            lyricsRevisionCache_ = lyricsRevision;
            lyricsSnapshotCache_ = lyrics_.Snapshot();
        }
        // Rich Presence follows bounded audio-clock notifications instead of paint.
        // This keeps lyric transitions alive while minimized and avoids invoking the
        // presence path thirty times a second just because the view repainted.
        model.lyrics = lyricsSnapshotCache_;
        model.lyricsCacheEnabled = settings_.Settings().lyricsCacheEnabled;
        model.lyricsOnlineEnabled = settings_.Settings().lyricsOnlineEnabled;
        model.lyricsFakeTimestampsEnabled = settings_.Settings().lyricsFakeTimestampsEnabled;
        const bool statsEnabled = settings_.Settings().statsEnabled;
        model.statsEnabled = statsEnabled;
        if (statsEnabled && statisticsCatalogCacheRevision_ != revision_) {
            statisticsCatalogCache_ = stats::BuildDashboardCatalog(playlists_.AllTracks());
            statisticsCatalogCacheRevision_ = revision_;
        }
        const auto statsSnapshot = stats_.Snapshot();
        if (statsEnabled &&
            (statsSnapshot != statisticsSnapshotCache_ ||
             statisticsDashboardCache_.period != statisticsPeriod_ ||
             statisticsDashboardCatalogRevision_ != revision_)) {
            statisticsSnapshotCache_ = statsSnapshot;
            statisticsDashboardCache_ = statsSnapshot
                ? stats::BuildDashboard(*statsSnapshot, statisticsCatalogCache_, statisticsPeriod_)
                : stats::DashboardData{.period = statisticsPeriod_};
            statisticsDashboardCatalogRevision_ = statisticsCatalogCacheRevision_;
            ++statisticsDashboardGeneration_;
        }

        auto& dashboard = model.statistics;
        if (dashboard.generation != statisticsDashboardGeneration_) {
            dashboard.period = statisticsDashboardCache_.period;
            dashboard.plays = statisticsDashboardCache_.totals.plays;
            dashboard.seconds = statisticsDashboardCache_.totals.seconds;
            dashboard.differentTracks = statisticsDashboardCache_.differentTracks;
            dashboard.tracks.clear();
            dashboard.tracks.reserve(statisticsDashboardCache_.topTracks.size());
            for (const auto& track : statisticsDashboardCache_.topTracks) {
                dashboard.tracks.push_back({track.id,
                                             core::Utf8ToWide(track.title, L"Unknown track"),
                                             core::Utf8ToWide(track.artist, L"Unknown artist"),
                                             track.filePath.wstring(),
                                             track.activity.plays, track.activity.seconds});
            }
            dashboard.artists.clear();
            dashboard.artists.reserve(statisticsDashboardCache_.topArtists.size());
            for (const auto& artist : statisticsDashboardCache_.topArtists) {
                dashboard.artists.push_back({core::Utf8ToWide(artist.name, L"Unknown artist"),
                                             artist.activity.plays, artist.activity.seconds,
                                             artist.trackCount});
            }
            dashboard.generation = statisticsDashboardGeneration_;
        }
        dashboard.tracksExpanded = statisticsTracksExpanded_;
        dashboard.artistsExpanded = statisticsArtistsExpanded_;
        constexpr std::size_t pageSize = 7;
        const auto maxPage = [](const std::size_t count) {
            return count == 0 ? 0 : (count - 1) / pageSize;
        };
        statisticsTracksPage_ = std::min(statisticsTracksPage_,
                                         maxPage(statisticsDashboardCache_.topTracks.size()));
        statisticsArtistsPage_ = std::min(statisticsArtistsPage_,
                                          maxPage(statisticsDashboardCache_.topArtists.size()));
        dashboard.tracksPage = statisticsTracksPage_;
        dashboard.artistsPage = statisticsArtistsPage_;
        dashboard.tracksCanPagePrevious = !statisticsTracksExpanded_ && statisticsTracksPage_ > 0;
        dashboard.tracksCanPageNext = !statisticsTracksExpanded_ &&
            (statisticsTracksPage_ + 1) * pageSize < dashboard.tracks.size();
        dashboard.artistsCanPagePrevious = !statisticsArtistsExpanded_ && statisticsArtistsPage_ > 0;
        dashboard.artistsCanPageNext = !statisticsArtistsExpanded_ &&
            (statisticsArtistsPage_ + 1) * pageSize < dashboard.artists.size();
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
    const bool filePreviewEnabled = settings_.Settings().filePreviewEnabled;
    const auto makeTrackView = [this, current, filePreviewEnabled](
                                    const auto& track, playlist::PlaylistId sourcePlaylistId) {
        ui::TrackView view{track.id, track.title, track.artist, track.album,
                            track.durationSeconds, track.bitrateKbps, track.id == selectedTrack_,
                           current != nullptr && current->id == track.id,
                           library::Track::IsAudioFile(track.filePath)};
        // Context-menu rename needs the backing filename even when preview and covers are off.
        view.filePath = track.filePath.wstring();
        view.lyricsDisabled = !view.filePath.empty() &&
            disabledLyricsSongs_.contains(
                lyrics::LyricsService::NormalizedTrackPath(track.filePath));
        view.sourcePlaylistId = sourcePlaylistId;
        return view;
    };

    // Library tree: optional Youtube downloader, flat All Music row, then folders / user.
    if (YoutubeFeatureOn()) {
        const std::size_t ytCount = youtubeView_.entries.size();
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
        // Resolve which Directory folder owns each child track once per snapshot; the
        // old per-track linear scan across every playlist was O(children x all tracks).
        std::unordered_map<library::TrackId, playlist::PlaylistId> ownerByTrack;
        {
            std::size_t ownedTrackCount = 0;
            for (const auto& playlist : playlists_.Playlists()) {
                if (playlist.kind == playlist::PlaylistKind::Directory) {
                    ownedTrackCount += playlist.trackIds.size();
                }
            }
            ownerByTrack.reserve(ownedTrackCount);
            for (const auto& playlist : playlists_.Playlists()) {
                if (playlist.kind != playlist::PlaylistKind::Directory) continue;
                for (const auto trackId : playlist.trackIds) {
                    // try_emplace keeps the first owning folder, matching the old scan.
                    ownerByTrack.try_emplace(trackId, playlist.id);
                }
            }
        }
        const auto ownerForTrack = [&ownerByTrack](library::TrackId trackId,
                                                   playlist::PlaylistId fallback) {
            const auto found = ownerByTrack.find(trackId);
            return found != ownerByTrack.end() ? found->second : fallback;
        };
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
    out.musicFolders.push_back(EffectiveMusicRoot().wstring());
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
    out.songRowLayout = settings_.Settings().songRowLayout;
    out.filePreviewEnabled = filePreviewEnabled;
    out.previewFitWindow = settings_.Settings().previewFitWindow;
    out.startAtStartup = settings_.Settings().startAtStartup;
    out.exitToTray = settings_.Settings().exitToTray;
    out.youtubeEnabled = settings_.Settings().youtubeEnabled;
    out.youtubeGrabberHotkeyAvailable = youtubeGrabberHotkeyAvailable_;
    out.youtubeGrabberHotkeyModifiers = settings_.Settings().youtubeGrabberHotkeyModifiers;
    out.youtubeGrabberHotkeyVirtualKey = settings_.Settings().youtubeGrabberHotkeyVirtualKey;
    out.discordEnabled = settings_.Settings().discordEnabled;
    out.discordShowArtist = settings_.Settings().discordShowArtist;
    out.discordSecondaryText = settings_.Settings().discordSecondaryText;
    out.discordFallbackToTotalStreams = settings_.Settings().discordFallbackToTotalStreams;
    out.discordShowGithubButton = settings_.Settings().discordShowGithubButton;
    out.lyricsAlignment = settings_.Settings().lyricsAlignment;
    out.moduleExpansionBehavior = settings_.Settings().moduleExpansionBehavior;
    out.windowResizeBehavior = settings_.Settings().windowResizeBehavior;
    out.moduleResizeBehavior = settings_.Settings().moduleResizeBehavior;
    out.youtubeBrowsing =
        YoutubeFeatureOn() && selectedPlaylist_ == playlist::YoutubePlaylistId;
    out.youtubeBusy = youtubeView_.busy;
    out.youtubeYtDlpInstalled = youtubeView_.ytDlpInstalled;
    out.youtubeFfmpegInstalled = youtubeView_.ffmpegInstalled;
    out.youtubeInstallingYtDlp = youtubeView_.installingYtDlp;
    out.youtubeInstallingFfmpeg = youtubeView_.installingFfmpeg;
    out.youtubeStatus = youtubeView_.status;
    out.youtubeChooserVisible = youtubeChooserVisible_;
    out.youtubeChooserEntryId = youtubeChooserEntryId_;
    out.youtubeProbe = youtubeView_.probe;
    out.youtubeDownloadSelection = youtubeDownloadSelection_;
    out.updateNotifierVisible = updateNotifierVisible_ && updateSnapshot_ &&
                                updateSnapshot_->updateAvailable;
    out.updateCurrentVersion = std::wstring(update::kCurrentVersion);
    out.updateLatestVersion = updateSnapshot_ ? updateSnapshot_->latestVersion : std::wstring{};
    out.skins.reserve(skins_.Skins().size());
    for (const auto& skin : skins_.Skins()) {
        out.skins.push_back({core::Utf8ToWide(skin.id, L"Unable to decode error text"),
                             core::Utf8ToWide(skin.name, L"Unable to decode error text"),
                             core::Utf8ToWide(skin.author, L"Unable to decode error text"),
                             skin.builtIn, skin.id == committedSkin_.id});
    }
    out.activeSkin = activeSkin_;
    out.moduleLayout = moduleLayout_;
    out.moduleLayoutWarning = moduleLayoutWarning_;
    out.focusedSkinColor = focusedSkinColor_;
    out.skinColorFocusRevision = skinColorFocusRevision_;
    out.focusedSkinElement = focusedSkinElement_;
    out.skinElementFocusRevision = skinElementFocusRevision_;
    fillLiveFields(out);

    cachedModel_ = out;
    cachedModelRevision_ = revision_;
}

} // namespace rivan
