// AppLibrary.cpp
// Asynchronous library scanning, restored-session state, playback navigation, and presence.
#include "App.h"

#include "config/SettingsManager.h"
#include "core/Text.h"
#include "stats/ListenStats.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace rivan {
namespace {

std::optional<std::uint64_t> ParseId(const std::string& text) noexcept {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

std::pair<std::wstring, std::wstring> App::LyricsMetadata(const library::Track& track) {
    std::wstring title = track.title.empty() ? track.filePath.stem().wstring() : track.title;
    std::wstring artist = track.artist;
    const auto clean = [](std::wstring value) {
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) value.pop_back();
        return value;
    };
    const auto separator = title.find(L" - ");
    if (artist.empty() && separator != std::wstring::npos && separator > 0 && separator + 3 < title.size()) {
        artist = clean(title.substr(0, separator));
        title = clean(title.substr(separator + 3));
    }
    if (artist.empty()) {
        const auto parent = track.filePath.parent_path().filename().wstring();
        if (!parent.empty() && parent != L"Music" && parent != L"music") artist = parent;
    }
    return {clean(std::move(title)), clean(std::move(artist))};
}

void App::SetDiscordEnabled(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.discordEnabled == enabled) return;
    settings.discordEnabled = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    discordPresencePublished_ = false;
    discordPresenceStateKey_.clear();
    discordPresenceBaseKey_.clear();
    discordPresenceTrackId_ = {};
    discordTrackTransitionPending_ = false;
    discordTrackTransitionPublished_ = false;
    discordTimestampRefreshPending_ = false;
    discordTimestampRefreshRequested_.store(false, std::memory_order_release);
    discordTrackTransitionLoadingObserved_.store(false, std::memory_order_release);
    discordPositionUpdatesEnabled_.store(
        enabled && settings.discordSecondaryText == config::DiscordSecondaryText::SyncLyrics,
        std::memory_order_release);
    discord_.SetEnabled(enabled);
    if (enabled) {
        UpdateDiscordPresence();
    } else {
        discord_.Clear();
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetDiscordShowArtist(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.discordShowArtist == enabled) return;
    settings.discordShowArtist = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    UpdateDiscordPresence();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetDiscordSecondaryText(config::DiscordSecondaryText mode) {
    auto settings = settings_.Settings();
    if (settings.discordSecondaryText == mode) return;
    settings.discordSecondaryText = mode;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    discordPositionUpdatesEnabled_.store(
        settings.discordEnabled && mode == config::DiscordSecondaryText::SyncLyrics,
        std::memory_order_release);
    UpdateDiscordPresence();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetDiscordFallbackToTotalStreams(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.discordFallbackToTotalStreams == enabled) return;
    settings.discordFallbackToTotalStreams = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    UpdateDiscordPresence();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetDiscordShowGithubButton(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.discordShowGithubButton == enabled) return;
    settings.discordShowGithubButton = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    UpdateDiscordPresence();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::StartLibraryScan() {
    if (scanThread_.joinable()) {
        scanThread_.request_stop();
        scanThread_.join();
    }
    std::vector<std::filesystem::path> roots;
    roots.push_back(EffectiveMusicRoot());
    for (const auto& root : settings_.Settings().additionalMusicRoots) roots.push_back(root);
    // Flag is written only on the UI thread; cleared in ApplyCompletedScan when the
    // result is applied.
    scanRunning_ = true;
    scanThread_ = std::jthread([this, roots = std::move(roots)](std::stop_token stop) {
        library::LibraryScanner scanner;
        library::LibraryScanResult result;
        try {
            result = scanner.Scan(std::span<const std::filesystem::path>(roots), stop);
        } catch (...) {
            // An exception escaping the worker would call std::terminate. Publish an
            // empty result through the normal completion path instead, so
            // ApplyCompletedScan still applies a consistent (empty) catalog and
            // shutdown proceeds exactly as after a completed scan.
            // result stays default-constructed (empty).
        }
        // Save probed durations (even partial) to disk so subsequent startups skip
        // the slow MF container open for unchanged files. Best effort only; a cache
        // write failure must never kill the worker thread.
        try {
            library::LibraryScanner::SaveDurationCache();
        } catch (...) {
        }
        if (stop.stop_requested()) return;
        std::scoped_lock lock(scanMutex_);
        completedScan_ = std::move(result);
    });
}

void App::ApplyCompletedScan() {
    // A download completed while the previous scan was still in flight; rescan once
    // the outstanding scan has been applied.
    if (downloadRescanPending_ && !scanRunning_) {
        downloadRescanPending_ = false;
        StartLibraryScan();
    }
    std::optional<library::LibraryScanResult> result;
    {
        std::scoped_lock lock(scanMutex_);
        if (!completedScan_) return;
        result = std::move(completedScan_);
        completedScan_.reset();
        // The scan's result is now applied on the UI thread; a pending download
        // rescan (checked at the top of this function) runs on the next pass.
        scanRunning_ = false;
    }
    // User playlists restore once, on the first scan application, so the per-track
    // metadata reads do not delay the initial scan start. They must be installed
    // before ApplyScan/ReplaceLibrary carries them across.
    if (!userPlaylistsLoaded_) {
        LoadUserPlaylists();
        userPlaylistsLoaded_ = true;
    }
    // Apply the completed catalog before exposing it to the UI.  The scan result is
    // moved into PlaylistManager here; no UI callback may observe the intermediate
    // vector replacement.
    const auto renames = playlists_.ApplyScan(*result);
    for (const auto& rename : renames) {
        // Migrate per-song stores so a renamed file keeps its play counts and lyrics.
        stats_.ApplyTrackRename(rename.oldId, rename.replacement.id, rename.oldPath,
                                rename.replacement.filePath);
        lyrics_.NotifyTrackRenamed(rename.oldId, rename.replacement.id, rename.oldPath,
                                   rename.replacement.filePath);
        if (activeTrack_ && activeTrack_->id == rename.oldId) {
            activeTrack_ = rename.replacement;
            selectedTrack_ = rename.replacement.id;
            // Re-resolve lyrics under the new id so ongoing lyric presence still matches.
            RefreshActiveLyrics();
        }
    }
    ApplyFolderOrderAfterScan();
    ApplyTrackOrderAfterScan();
    RestoreSessionAfterScan();
    if (!renames.empty()) {
        // Reflect renamed tracks in Rich Presence right away, even while paused.
        UpdateDiscordPresence();
    }
    ++revision_;
}

void App::RestoreSessionAfterScan() {
    if (playlists_.Playlists().empty()) {
        queue_.Clear();
        restored_ = true;
        return;
    }
    if (!restored_) {
        if (const auto id = ParseId(settings_.Session().selectedPlaylist); id && playlists_.FindPlaylist(*id)) {
            selectedPlaylist_ = *id;
        }
    }
    if (playlists_.FindPlaylist(selectedPlaylist_) == nullptr) selectedPlaylist_ = playlists_.Playlists().front().id;

    auto tracks = playlists_.ResolveTracksRecursive(selectedPlaylist_);
    std::optional<std::size_t> selectedIndex;
    if (!restored_) {
        if (const auto id = ParseId(settings_.Session().selectedTrack)) {
            const auto found = std::find_if(tracks.begin(), tracks.end(), [id](const auto& track) { return track.id == *id; });
            if (found != tracks.end()) selectedIndex = static_cast<std::size_t>(std::distance(tracks.begin(), found));
        }
    } else if (activeTrack_) {
        // A manual rescan (library edit) rebuilt the catalog; anchor the queue on the
        // currently playing track so end-of-track and Next/Previous navigation do not
        // jump back to the top. If the playing file was deleted/moved, fall back below.
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [this](const auto& track) { return track.id == activeTrack_->id; });
        if (found != tracks.end()) {
            selectedIndex = static_cast<std::size_t>(std::distance(tracks.begin(), found));
        }
    }
    queue_.SetTracks(std::move(tracks), selectedIndex);
    queue_.SetShuffle(settings_.Session().shuffle);
    queue_.SetRepeat(ToQueueRepeat(settings_.Session().repeat));
    if (selectedIndex && queue_.Current()) {
        if (!restored_) {
            // Startup session restore: re-establish playback state and seek position.
            selectedTrack_ = queue_.Current()->id;
            activeTrack_ = *queue_.Current();
            stats_.SetActiveTrack(activeTrack_);
            const auto metadata = LyricsMetadata(*activeTrack_);
            lyrics_.Request(activeTrack_->id, metadata.first, metadata.second,
                            activeTrack_->album, activeTrack_->durationSeconds,
                            activeTrack_->filePath);
            audio_.Load(queue_.Current()->filePath);
            if (settings_.Session().positionMilliseconds != 0) {
                audio_.Seek(std::chrono::milliseconds(settings_.Session().positionMilliseconds));
            }
        } else {
            // Manual rescan: keep the queue anchored on the playing track without
            // reloading audio (it is already playing and must not restart).
            selectedTrack_ = queue_.Current()->id;
            activeTrack_ = *queue_.Current();
            stats_.SetActiveTrack(activeTrack_);
        }
    }
    SyncPlaybackSession();
    restored_ = true;
}

void App::HandleAudioSignals() {
    if (endOfStream_.exchange(false, std::memory_order_acq_rel)) PlayNavigation(queue_.OnEndOfStream());
    if (audioChanged_.exchange(false, std::memory_order_acq_rel)) {
        if (discordTimestampRefreshRequested_.exchange(false, std::memory_order_acq_rel)) {
            discordTimestampRefreshPending_ = true;
        }
        ++revision_;
        UpdateDiscordPresence();
        if (window_) window_->Refresh();
    } else if (audioPositionChanged_.exchange(false, std::memory_order_acq_rel)) {
        // This comes from the audio clock rather than paint, so a hidden/minimized
        // window still discovers a newly active synced lyric line.
        UpdateDiscordPresence();
    }
}

void App::NotifyAudioSignal() {
    const HWND window = audioNotificationWindow_.load(std::memory_order_acquire);
    if (!window) return;
    (void)PostMessageW(window, ui::kAudioSignalMessage, 0, 0);
}

void App::PlayNavigation(const playlist::QueueNavigation& navigation, bool startPlayback) {
    if (!navigation || navigation.track == nullptr) return;
    // Same-track restart (repeat-one via OnEndOfStream, or PlayPause restart from
    // stopped) must earn a fresh listening session; the 500ms state sampler cannot see
    // the EndOfStream -> Loading -> Playing collapse. Different-track Advanced
    // navigation is handled by the sampler's path comparison.
    if (navigation.action == playlist::QueueNavigationAction::Restarted) stats_.OnPlaybackRestarted();
    selectedTrack_ = navigation.track->id;
    activeTrack_ = *navigation.track;
    stats_.SetActiveTrack(activeTrack_);
    discordTrackTransitionPending_ = startPlayback;
    discordTrackTransitionPublished_ = false;
    discordTimestampRefreshPending_ = false;
    discordTrackTransitionLoadingObserved_.store(false, std::memory_order_release);
    audio_.Load(navigation.track->filePath);
    SyncPlaybackSession();
    const auto metadata = LyricsMetadata(*navigation.track);
    lyrics_.Request(navigation.track->id, metadata.first, metadata.second,
                    navigation.track->album, navigation.track->durationSeconds,
                    navigation.track->filePath);
    if (startPlayback) audio_.Play();
    UpdateDiscordPresence();
    ++revision_;
}

void App::UpdateDiscordPresence() {
    if (!settings_.Settings().discordEnabled) return;
    const auto* track = activeTrack_ ? &*activeTrack_ : nullptr;
    const auto live = audio_.Live();
    discord::PresenceActivity activity;
    if (discordTimestampRefreshRequested_.exchange(false, std::memory_order_acq_rel)) {
        discordTimestampRefreshPending_ = true;
    }
    if (discordTrackTransitionPending_ && live.state == audio::PlaybackState::Error) {
        discordTrackTransitionPending_ = false;
        discordTrackTransitionPublished_ = false;
        discordTimestampRefreshPending_ = false;
    }
    // Load/Play is asynchronous and Live() can still describe the previous track as
    // Playing. Do not spend an RPC update on a placeholder or stale audio clock: wait
    // until the audio worker has reported Loading, then publish once its new clock runs.
    bool refreshTimestamp = false;
    if (discordTrackTransitionPending_ && live.state != audio::PlaybackState::Error) {
        if (!discordTrackTransitionLoadingObserved_.load(std::memory_order_acquire) ||
            live.state != audio::PlaybackState::Playing) {
            return;
        }
        discordTrackTransitionPending_ = false;
        discordTrackTransitionPublished_ = false;
    }
    if (live.state == audio::PlaybackState::Playing && discordTimestampRefreshPending_) {
        refreshTimestamp = true;
        discordTimestampRefreshPending_ = false;
    }
    if (track == nullptr || live.state != audio::PlaybackState::Playing) {
        // Idle/paused presence; dedupe so the 30 Hz paint does not clear repeatedly.
        constexpr std::string_view kClearKey = "clear";
        if (discordPresencePublished_ && discordPresenceStateKey_ == kClearKey) return;
        discordPresencePublished_ = true;
        discordPresenceStateKey_ = std::string(kClearKey);
        discordPresenceBaseKey_.clear();
        discordPresenceTrackId_ = {};
        discordTrackTransitionPublished_ = false;
        discordTimestampRefreshPending_ = false;
        discord_.SetActivity(activity);
        return;
    }
    // Cache may be stale when lyrics arrive mid-track from the message loop before the
    // next paint; refresh here so the verse appears immediately.
    const auto lyricsRevision = lyrics_.Revision();
    if (lyricsRevision != lyricsRevisionCache_) {
        lyricsRevisionCache_ = lyricsRevision;
        lyricsSnapshotCache_ = lyrics_.Snapshot();
    }
    // Refresh statistics snapshot for total streams lookup.
    statisticsSnapshotCache_ = stats_.Snapshot();

    activity.hasTrack = true;
    activity.playing = true;
    activity.details = core::WideToUtf8(track->title.empty() ? track->filePath.stem().wstring() : track->title);
    const auto secondaryMode = settings_.Settings().discordSecondaryText;
    const bool showArtist = settings_.Settings().discordShowArtist;
    const bool fallbackToTotalStreams = settings_.Settings().discordFallbackToTotalStreams;

    std::string stateLine;
    std::string imageText;

    const auto totalStreamsText = [&]() {
        const std::string sectionName = stats::SongSectionName(track->id);
        const auto* entity = statisticsSnapshotCache_
            ? statisticsSnapshotCache_->Find(sectionName)
            : nullptr;
        return std::string("Total streams: ") +
               std::to_string(entity != nullptr ? entity->lifetime.plays : 0);
    };

    if (secondaryMode == config::DiscordSecondaryText::SyncLyrics) {
        std::string verse;
        if (!lyricsSnapshotCache_.loading && lyricsSnapshotCache_.available &&
            lyricsSnapshotCache_.trackId == track->id && lyricsSnapshotCache_.document.synced) {
            const double positionSec = std::chrono::duration<double>(live.position).count();
            const auto& lines = lyricsSnapshotCache_.document.lines;
            std::size_t activeLine = 0;
            bool found = false;
            for (std::size_t index = 0; index < lines.size(); ++index) {
                if (lines[index].timestampSeconds >= 0.0 &&
                    lines[index].timestampSeconds <= positionSec) {
                    activeLine = index;
                    found = true;
                }
            }
            if (found && !lines[activeLine].text.empty()) {
                verse = core::WideToUtf8(lines[activeLine].text);
            }
        }
        // The current timed verse is Rich Presence artwork text. Total streams is
        // only a fallback for tracks without an active synced line.
        imageText = !verse.empty() ? std::move(verse)
                                  : (fallbackToTotalStreams ? totalStreamsText() : std::string{});
        if (showArtist) {
            stateLine = core::WideToUtf8(track->artist.empty() ? L"Unknown artist" : track->artist);
        }
    } else if (secondaryMode == config::DiscordSecondaryText::TotalStreams) {
        // Total streams: show lifetime play count in the artwork tooltip (like lyrics).
        imageText = totalStreamsText();
        if (showArtist) {
            stateLine = core::WideToUtf8(track->artist.empty() ? L"Unknown artist" : track->artist);
        }
    } else { // Off
        if (showArtist) {
            stateLine = core::WideToUtf8(track->artist.empty() ? L"Unknown artist" : track->artist);
        }
    }

    activity.state = std::move(stateLine);
    activity.imageText = std::move(imageText);
    activity.showGithubButton = settings_.Settings().discordShowGithubButton;
    const auto nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto positionSec = std::chrono::duration_cast<std::chrono::seconds>(live.position).count();
    const auto durationSec = std::chrono::duration_cast<std::chrono::seconds>(live.duration).count();
    activity.startUnix = nowUnix - std::max<std::int64_t>(0, positionSec);
    if (durationSec > 0) activity.endUnix = activity.startUnix + durationSec;
    // Timestamps and playback position remain outside app-side dedupe. A new library
    // id still forces a track update when two tracks share metadata.
    const std::string baseKey = std::to_string(track->id) + "\x1f" + activity.details + "\x1f" +
                                activity.state + "\x1f" +
                                (activity.showGithubButton ? "1" : "0");
    const std::string key = baseKey + "\x1f" + activity.imageText;
    if (!refreshTimestamp && discordPresencePublished_ && discordPresenceStateKey_ == key) return;
    const bool lyricOnlyUpdate = !refreshTimestamp && discordPresencePublished_ &&
                                  discordPresenceTrackId_ == track->id &&
                                  discordPresenceBaseKey_ == baseKey;
    discordPresencePublished_ = true;
    discordPresenceStateKey_ = key;
    discordPresenceBaseKey_ = baseKey;
    discordPresenceTrackId_ = track->id;
    discord_.SetActivity(std::move(activity), lyricOnlyUpdate
                                                  ? discord::ActivityUpdateKind::Lyric
                                                  : discord::ActivityUpdateKind::Track);
}

} // namespace rivan
