// AppLibrary.cpp
// Asynchronous library scanning, restored-session state, playback navigation, and presence.
#include "App.h"

#include "core/Text.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace rivan {
namespace {

std::pair<std::wstring, std::wstring> LyricsMetadata(const library::Track& track) {
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

std::optional<std::uint64_t> ParseId(const std::string& text) noexcept {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

void App::SetDiscordEnabled(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.discordEnabled == enabled) return;
    settings.discordEnabled = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
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

void App::SetDiscordShowImageText(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.discordShowImageText == enabled) return;
    settings.discordShowImageText = enabled;
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
    roots.push_back(settings_.Settings().musicRoot);
    for (const auto& root : settings_.Settings().additionalMusicRoots) roots.push_back(root);
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
    std::optional<library::LibraryScanResult> result;
    {
        std::scoped_lock lock(scanMutex_);
        if (!completedScan_) return;
        result = std::move(completedScan_);
        completedScan_.reset();
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
    playlists_.ApplyScan(*result);
    ApplyFolderOrderAfterScan();
    ApplyTrackOrderAfterScan();
    RestoreSessionAfterScan();
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
            const auto metadata = LyricsMetadata(*activeTrack_);
            lyrics_.Request(activeTrack_->id, metadata.first, metadata.second,
                            activeTrack_->album, activeTrack_->durationSeconds);
            audio_.Load(queue_.Current()->filePath);
            if (settings_.Session().positionMilliseconds != 0) {
                audio_.Seek(std::chrono::milliseconds(settings_.Session().positionMilliseconds));
            }
        } else {
            // Manual rescan: keep the queue anchored on the playing track without
            // reloading audio (it is already playing and must not restart).
            selectedTrack_ = queue_.Current()->id;
            activeTrack_ = *queue_.Current();
        }
    }
    restored_ = true;
}

void App::HandleAudioSignals() {
    if (endOfStream_.exchange(false, std::memory_order_acq_rel)) PlayNavigation(queue_.OnEndOfStream());
    if (audioChanged_.exchange(false, std::memory_order_acq_rel)) {
        ++revision_;
        UpdateDiscordPresence();
        if (window_) window_->Refresh();
    }
}

void App::NotifyAudioSignal() {
    const HWND window = audioNotificationWindow_.load(std::memory_order_acquire);
    if (!window) return;
    (void)PostMessageW(window, ui::kAudioSignalMessage, 0, 0);
}

void App::PlayNavigation(const playlist::QueueNavigation& navigation, bool startPlayback) {
    if (!navigation || navigation.track == nullptr) return;
    selectedTrack_ = navigation.track->id;
    activeTrack_ = *navigation.track;
    audio_.Load(navigation.track->filePath);
    const auto metadata = LyricsMetadata(*navigation.track);
    lyrics_.Request(navigation.track->id, metadata.first, metadata.second,
                    navigation.track->album, navigation.track->durationSeconds);
    if (startPlayback) audio_.Play();
    UpdateDiscordPresence();
    ++revision_;
}

void App::UpdateDiscordPresence() {
    if (!settings_.Settings().discordEnabled) return;
    const auto* track = activeTrack_ ? &*activeTrack_ : nullptr;
    const auto live = audio_.Live();
    discord::PresenceActivity activity;
    if (track == nullptr || live.state != audio::PlaybackState::Playing) {
        discord_.SetActivity(activity);
        return;
    }
    activity.hasTrack = true;
    activity.playing = true;
    activity.details = core::WideToUtf8(track->title.empty() ? track->filePath.stem().wstring() : track->title);
    if (settings_.Settings().discordShowArtist) activity.state = core::WideToUtf8(track->artist.empty() ? L"Unknown artist" : track->artist);
    activity.showImageText = settings_.Settings().discordShowImageText;
    activity.showGithubButton = settings_.Settings().discordShowGithubButton;
    const auto nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto positionSec = std::chrono::duration_cast<std::chrono::seconds>(live.position).count();
    const auto durationSec = std::chrono::duration_cast<std::chrono::seconds>(live.duration).count();
    activity.startUnix = nowUnix - std::max<std::int64_t>(0, positionSec);
    if (durationSec > 0) activity.endUnix = activity.startUnix + durationSec;
    discord_.SetActivity(std::move(activity));
}

} // namespace rivan
