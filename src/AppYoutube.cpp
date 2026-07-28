// AppYoutube.cpp
// YouTube settings, search, downloads, and local download-library integration.
#include "App.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace rivan {

bool App::YoutubeFeatureOn() const noexcept {
    return settings_.Settings().youtubeEnabled;
}

void App::SetYoutubeEnabled(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.youtubeEnabled == enabled) return;
    settings.youtubeEnabled = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);

    if (!enabled) {
        youtube_.Reset();
        youtubeView_ = youtube_.Snapshot();
        youtubeSelectedResult_ = 0;
        pendingPlayYoutubeId_ = 0;
        if (selectedPlaylist_ == playlist::YoutubePlaylistId) {
            selectedPlaylist_ = playlist::AllMusicPlaylistId;
            if (playlists_.FindPlaylist(selectedPlaylist_)) {
                queue_.SetTracks(playlists_.ResolveTracksRecursive(selectedPlaylist_),
                                 std::nullopt);
            }
        }
    } else {
        std::error_code ec;
        std::filesystem::create_directories(
            youtube::YoutubeService::DownloadDirectory(settings_.Settings().musicRoot), ec);
        youtube_.RefreshToolStatus();
        youtubeView_ = youtube_.Snapshot();
        if (!youtubeView_.ytDlpInstalled) {
            youtubeView_.status = L"Install yt-dlp in Preferences → General";
        } else {
            youtubeView_.status = L"Ready — search or paste a YouTube URL";
        }
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetYoutubeMusicSearch(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.youtubeMusicSearch == enabled) return;
    settings.youtubeMusicSearch = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    // Drop current result list so UI does not mix YT and YTM hits.
    if (YoutubeFeatureOn() && selectedPlaylist_ == playlist::YoutubePlaylistId) {
        youtubeSelectedResult_ = 0;
        pendingPlayYoutubeId_ = 0;
        ShowYoutubeLocalLibrary();
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetYoutubeDownloadMode(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    auto settings = settings_.Settings();
    if (settings.youtubeDownloadMode == mode) return;
    settings.youtubeDownloadMode = mode;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetYoutubeAudioQuality(int quality) {
    if (quality < 0) quality = 0;
    if (quality > 9) quality = 9;
    auto settings = settings_.Settings();
    if (settings.youtubeAudioQuality == quality) return;
    settings.youtubeAudioQuality = quality;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetYoutubeMp4VideoQuality(int quality) {
    if (quality < 0) quality = 0;
    if (quality > 5) quality = 5;
    auto settings = settings_.Settings();
    if (settings.youtubeMp4VideoQuality == quality) return;
    settings.youtubeMp4VideoQuality = quality;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::InstallYoutubeTool(bool ytDlp) {
    youtube_.InstallTool(ytDlp ? youtube::YoutubeTool::YtDlp : youtube::YoutubeTool::Ffmpeg);
    youtubeView_ = youtube_.Snapshot();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SubmitYoutubeQuery(std::wstring query) {
    if (!YoutubeFeatureOn()) return;
    if (selectedPlaylist_ != playlist::YoutubePlaylistId) {
        selectedPlaylist_ = playlist::YoutubePlaylistId;
    }
    youtubeSelectedResult_ = 0;
    pendingPlayYoutubeId_ = 0;
    youtube_.SubmitQuery(std::move(query), settings_.Settings().youtubeMusicSearch);
    youtubeView_ = youtube_.Snapshot();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetYoutubeSearchPage(std::size_t page) {
    if (!YoutubeFeatureOn()) return;
    if (!youtube_.SetSearchPage(page)) return;
    youtubeView_ = youtube_.Snapshot();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::ActivateYoutubeResult(std::uint64_t id) {
    if (!YoutubeFeatureOn()) return;
    youtubeSelectedResult_ = id;

    const auto snapshot = youtube_.Snapshot();
    const auto found = std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
                                    [id](const youtube::YoutubeEntry& e) {
                                        return e.id == id;
                                    });
    if (found == snapshot.entries.end()) return;

    std::error_code ec;
    if (!found->localPath.empty() && std::filesystem::is_regular_file(found->localPath, ec)) {
        library::Track track = library::Track::FromFile(found->localPath);
        track.title = found->title.empty() ? track.title : found->title;
        queue_.SetTracks(std::vector<library::Track>{track}, 0);
        PlayNavigation(queue_.Play(0));
        ++revision_;
        if (window_) window_->Refresh();
        return;
    }

    pendingPlayYoutubeId_ = id;
    const auto& s = settings_.Settings();
    youtube_.Download(id, s.musicRoot, s.youtubeAudioQuality, s.youtubeDownloadMode,
                      s.youtubeMp4VideoQuality, s.youtubeMusicSearch);
    youtubeView_ = youtube_.Snapshot();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::ShowYoutubeLocalLibrary() {
    youtube_.RefreshToolStatus();
    const auto tools = youtube_.Snapshot();
    const auto directory =
        youtube::YoutubeService::DownloadDirectory(settings_.Settings().musicRoot);
    std::error_code ec;
    youtubeView_.entries.clear();
    if (std::filesystem::is_directory(directory, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            if (!library::Track::IsSupportedFile(entry.path())) continue;
            youtube::YoutubeEntry row;
            row.localPath = entry.path();
            // Prefer "Song Name [videoId]" stem without trailing [id] when present.
            std::wstring stem = entry.path().stem().wstring();
            if (stem.size() > 3 && stem.back() == L']') {
                const auto open = stem.rfind(L" [");
                if (open != std::wstring::npos && stem.size() - open > 3) {
                    row.videoId = stem.substr(open + 2, stem.size() - open - 3);
                    stem.resize(open);
                }
            }
            row.title = stem.empty() ? entry.path().stem().wstring() : stem;
            row.id = library::Track::FromFile(entry.path()).id;
            youtubeView_.entries.push_back(std::move(row));
        }
    }
    youtubeView_.busy = false;
    youtubeView_.job = youtube::YoutubeJobKind::Idle;
    youtubeView_.ytDlpInstalled = tools.ytDlpInstalled;
    youtubeView_.ffmpegInstalled = tools.ffmpegInstalled;
    youtubeView_.installingYtDlp = tools.installingYtDlp;
    youtubeView_.installingFfmpeg = tools.installingFfmpeg;
    youtubeView_.searchIsPaged = false;
    youtubeView_.searchPage = 0;
    youtubeView_.searchPageCount = 1;
    if (youtubeView_.entries.empty()) {
        youtubeView_.status = youtubeView_.ytDlpInstalled
                                  ? L"Search YouTube or paste a URL"
                                  : L"Install yt-dlp in Preferences → General";
    } else {
        youtubeView_.status =
            std::to_wstring(youtubeView_.entries.size()) + L" downloaded track(s)";
    }
    ++youtubeView_.generation;
}

void App::OnYoutubeServiceUpdated() {
    if (!YoutubeFeatureOn()) return;
    ApplyYoutubeSnapshot(true, pendingPlayYoutubeId_);
}

void App::ApplyYoutubeSnapshot(bool playIfReady, std::uint64_t playEntryId) {
    const auto previousGen = youtubeView_.generation;
    const bool wasBusy = youtubeView_.busy;
    const auto previousJob = youtubeView_.job;
    youtubeView_ = youtube_.Snapshot();
    if (youtubeView_.generation == previousGen && !playIfReady) return;

    // Downloads land in musicRoot/Youtube like any other folder — rescan catalog when
    // a download job finishes with at least one file on disk.
    if (wasBusy && !youtubeView_.busy && previousJob == youtube::YoutubeJobKind::Download) {
        bool downloaded = false;
        for (const auto& entry : youtubeView_.entries) {
            if (entry.localPath.empty() || entry.failed) continue;
            std::error_code ec;
            if (std::filesystem::is_regular_file(entry.localPath, ec)) {
                downloaded = true;
                break;
            }
        }
        if (downloaded) {
            restored_ = true;
            StartLibraryScan();
        }
    }

    if (playIfReady && playEntryId != 0) {
        const auto found = std::find_if(
            youtubeView_.entries.begin(), youtubeView_.entries.end(),
            [playEntryId](const youtube::YoutubeEntry& e) { return e.id == playEntryId; });
        if (found != youtubeView_.entries.end() && !found->localPath.empty() &&
            !found->downloading) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(found->localPath, ec)) {
                library::Track track = library::Track::FromFile(found->localPath);
                track.title = found->title.empty() ? track.title : found->title;
                queue_.SetTracks(std::vector<library::Track>{track}, 0);
                PlayNavigation(queue_.Play(0));
                pendingPlayYoutubeId_ = 0;
            }
        } else if (found != youtubeView_.entries.end() && found->failed) {
            pendingPlayYoutubeId_ = 0;
        }
    }

    ++revision_;
    if (window_) window_->Refresh();
}

} // namespace rivan
