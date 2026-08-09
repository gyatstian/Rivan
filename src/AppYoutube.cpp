// AppYoutube.cpp
// YouTube settings, search, downloads, and local download-library integration.
#include "App.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace rivan {
namespace {

template <typename Format>
void CycleFormatId(std::wstring& selected, const std::vector<Format>& formats, int direction) {
    if (formats.empty()) {
        selected.clear();
        return;
    }
    std::size_t index = 0;
    const auto found = std::find_if(formats.begin(), formats.end(),
                                    [&selected](const Format& format) {
                                        return format.formatId == selected;
                                    });
    if (found != formats.end()) index = static_cast<std::size_t>(found - formats.begin());
    const auto count = static_cast<long long>(formats.size());
    long long next = static_cast<long long>(index) + static_cast<long long>(direction);
    next %= count;
    if (next < 0) next += count;
    index = static_cast<std::size_t>(next);
    selected = formats[index].formatId;
}

void EnsureYoutubeSelection(youtube::YoutubeDownloadSelection& selection,
                            const youtube::YoutubeProbe& probe) {
    const auto hasVideo = std::any_of(
        probe.videoFormats.begin(), probe.videoFormats.end(),
        [&selection](const auto& format) { return format.formatId == selection.videoFormatId; });
    const auto hasAudio = std::any_of(
        probe.audioFormats.begin(), probe.audioFormats.end(),
        [&selection](const auto& format) { return format.formatId == selection.audioFormatId; });
    if (!hasVideo && !probe.videoFormats.empty()) {
        const auto preferred = std::find_if(
            probe.videoFormats.begin(), probe.videoFormats.end(), [&selection](const auto& format) {
                return format.extension == selection.preferredVideoExtension &&
                       (selection.videoHeight <= 0 || format.height == selection.videoHeight);
            });
        const auto& selected = preferred != probe.videoFormats.end()
                                   ? *preferred
                                   : probe.videoFormats.front();
        selection.videoFormatId = selected.formatId;
    }
    if (const auto video = std::find_if(
            probe.videoFormats.begin(), probe.videoFormats.end(),
            [&selection](const auto& format) { return format.formatId == selection.videoFormatId; });
        video != probe.videoFormats.end()) {
        selection.videoHeight = video->height;
        selection.videoFps = video->fps;
        selection.preferredVideoExtension = video->extension;
    } else {
        selection.videoHeight = 0;
        selection.videoFps = 0.0;
    }
    if (!hasAudio && !probe.audioFormats.empty()) {
        const auto preferred = std::find_if(
            probe.audioFormats.begin(), probe.audioFormats.end(), [&selection](const auto& format) {
                return format.extension == selection.preferredAudioExtension &&
                       (selection.preferredAudioBitrate <= 0 ||
                        static_cast<int>(std::lround(format.abr)) == selection.preferredAudioBitrate);
            });
        const auto& selected = preferred != probe.audioFormats.end()
                                   ? *preferred
                                   : probe.audioFormats.front();
        selection.audioFormatId = selected.formatId;
    }
    if (const auto audio = std::find_if(
            probe.audioFormats.begin(), probe.audioFormats.end(),
            [&selection](const auto& format) { return format.formatId == selection.audioFormatId; });
        audio != probe.audioFormats.end()) {
        selection.preferredAudioExtension = audio->extension;
        selection.preferredAudioBitrate = static_cast<int>(std::lround(audio->abr));
    }
    selection.audioQuality = std::clamp(selection.audioQuality, 0, 9);
}

youtube::YoutubeDownloadSelection YoutubeSelectionFromSettings(
    const config::AppSettings& settings) {
    youtube::YoutubeDownloadSelection selection;
    selection.kind = settings.youtubeDownloadKind == 0
                         ? youtube::YoutubeDownloadKind::Video
                         : youtube::YoutubeDownloadKind::AudioOnly;
    selection.audioOutput = static_cast<youtube::YoutubeAudioOutputFormat>(
        std::clamp(settings.youtubeAudioOutputFormat, 0, 5));
    selection.audioQuality = std::clamp(settings.youtubeAudioQuality, 0, 9);
    selection.videoHeight = settings.youtubeVideoHeight;
    selection.videoFps = settings.youtubeVideoFps;
    selection.preferredVideoExtension = std::wstring(settings.youtubeVideoExtension.begin(),
                                                     settings.youtubeVideoExtension.end());
    selection.preferredAudioExtension = std::wstring(settings.youtubeAudioExtension.begin(),
                                                     settings.youtubeAudioExtension.end());
    selection.preferredAudioBitrate = settings.youtubeAudioBitrate;
    return selection;
}

std::string NarrowAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        result.push_back(character >= 0 && character <= 0x7f
                             ? static_cast<char>(character)
                             : '?');
    }
    return result;
}

} // namespace

bool App::YoutubeFeatureOn() const noexcept {
    return settings_.Settings().youtubeEnabled;
}

void App::PersistYoutubeChooserSelection() {
    auto settings = settings_.Settings();
    settings.youtubeDownloadKind = youtubeDownloadSelection_.kind == youtube::YoutubeDownloadKind::Video ? 0 : 1;
    settings.youtubeAudioOutputFormat = static_cast<int>(youtubeDownloadSelection_.audioOutput);
    settings.youtubeAudioQuality = std::clamp(youtubeDownloadSelection_.audioQuality, 0, 9);
    settings.youtubeVideoHeight = std::max(0, youtubeDownloadSelection_.videoHeight);
    settings.youtubeVideoFps = std::max(0, static_cast<int>(std::lround(youtubeDownloadSelection_.videoFps)));
    settings.youtubeVideoExtension = NarrowAscii(youtubeDownloadSelection_.preferredVideoExtension);
    settings.youtubeAudioExtension = NarrowAscii(youtubeDownloadSelection_.preferredAudioExtension);
    settings.youtubeAudioBitrate = std::max(0, youtubeDownloadSelection_.preferredAudioBitrate);
    std::string error;
    if (settings_.SetSettings(std::move(settings), &error)) (void)settings_.SaveSettings(&error);
}

void App::SetYoutubeEnabled(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.youtubeEnabled == enabled) return;
    const bool grabberHotkeyAvailable = !enabled || !window_ ||
        window_->UpdateYoutubeGrabberHotkey(settings.youtubeGrabberHotkeyModifiers,
                                             settings.youtubeGrabberHotkeyVirtualKey);
    settings.youtubeEnabled = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);

    if (!enabled) {
        if (window_) (void)window_->UpdateYoutubeGrabberHotkey(0, 0);
        youtubeGrabberHotkeyAvailable_ = false;
        youtube_.Reset();
        youtubeView_ = youtube_.Snapshot();
        youtubeSelectedResult_ = 0;
        youtubeChooserVisible_ = false;
        youtubeChooserEntryId_ = 0;
        youtubeDownloadSelection_ = {};
        pendingYoutubeGrab_ = false;
        if (selectedPlaylist_ == playlist::YoutubePlaylistId) {
            selectedPlaylist_ = playlist::AllMusicPlaylistId;
            if (playlists_.FindPlaylist(selectedPlaylist_)) {
                queue_.SetTracks(playlists_.ResolveTracksRecursive(selectedPlaylist_),
                                 std::nullopt);
            }
        }
    } else {
        youtubeGrabberHotkeyAvailable_ = grabberHotkeyAvailable;
        std::error_code ec;
        std::filesystem::create_directories(
            youtube::YoutubeService::DownloadDirectory(settings_.Settings().musicRoot), ec);
        youtube_.RefreshToolStatus();
        youtubeView_ = youtube_.Snapshot();
        if (!grabberHotkeyAvailable) {
            youtubeView_.status = L"YouTube grabber hotkey unavailable — choose another in Preferences → Online";
        } else if (!youtubeView_.ytDlpInstalled) {
            youtubeView_.status = L"Install yt-dlp in Preferences → Online";
        } else {
            youtubeView_.status = L"Ready — search or paste a YouTube URL";
        }
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::GrabYoutubeLink(std::wstring url) {
    if (!YoutubeFeatureOn() || !youtube::YoutubeService::LooksLikeYoutubeUrl(url)) return;
    pendingYoutubeGrab_ = true;
    SubmitYoutubeQuery(std::move(url));
    youtubeChooserVisible_ = true;
    youtubeChooserEntryId_ = 0;
    ++revision_;
    if (window_) {
        window_->RevealYoutubeChooser();
        window_->Refresh();
    }
}

bool App::SetYoutubeGrabberHotkey(std::uint32_t modifiers,
                                  std::uint32_t virtualKey) {
    auto settings = settings_.Settings();
    settings.youtubeGrabberHotkeyModifiers = modifiers;
    settings.youtubeGrabberHotkeyVirtualKey = virtualKey;
    std::string error;
    if (!config::SettingsManager::Validate(settings, &error)) return false;
    if (YoutubeFeatureOn() && window_ &&
        !window_->UpdateYoutubeGrabberHotkey(modifiers, virtualKey)) {
        return false;
    }
    if (!settings_.SetSettings(settings, &error)) return false;
    youtubeGrabberHotkeyAvailable_ = true;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
    return true;
}

void App::InstallYoutubeTool(bool ytDlp) {
    youtube_.InstallTool(ytDlp ? youtube::YoutubeTool::YtDlp : youtube::YoutubeTool::Ffmpeg);
    youtubeView_ = youtube_.Snapshot();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SubmitYoutubeQuery(std::wstring query) {
    if (!YoutubeFeatureOn()) return;
    const bool keepGrabRequest = pendingYoutubeGrab_;
    pendingYoutubeGrab_ = false;
    if (selectedPlaylist_ != playlist::YoutubePlaylistId) {
        selectedPlaylist_ = playlist::YoutubePlaylistId;
    }
    youtubeSelectedResult_ = 0;
    youtubeChooserVisible_ = false;
    youtubeChooserEntryId_ = 0;
    youtubeDownloadSelection_ = YoutubeSelectionFromSettings(settings_.Settings());
    youtube_.SubmitQuery(query);
    pendingYoutubeGrab_ = keepGrabRequest;
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
        youtubeChooserVisible_ = false;
        youtubeChooserEntryId_ = 0;
        library::Track track = library::Track::FromFile(found->localPath);
        track.title = found->title.empty() ? track.title : found->title;
        queue_.SetTracks(std::vector<library::Track>{track}, 0);
        PlayNavigation(queue_.Play(0));
        ++revision_;
        if (window_) window_->Refresh();
        return;
    }

    youtubeChooserVisible_ = true;
    youtubeChooserEntryId_ = id;
    youtubeDownloadSelection_ = YoutubeSelectionFromSettings(settings_.Settings());
    const bool probingThisEntry = snapshot.job == youtube::YoutubeJobKind::Probe &&
                                  snapshot.probeEntryId == id;
    const bool probeReady = snapshot.job == youtube::YoutubeJobKind::Idle &&
                            snapshot.probe && snapshot.probeEntryId == id;
    if (probeReady) {
        EnsureYoutubeSelection(youtubeDownloadSelection_, *snapshot.probe);
    } else if (!probingThisEntry) {
        youtube_.Probe(id);
    }
    youtubeView_ = youtube_.Snapshot();
    ++revision_;
    if (window_) {
        window_->RevealYoutubeChooser();
        window_->Refresh();
    }
}

void App::SetYoutubeChooserVisible(bool visible) {
    if (visible) {
        if (youtubeChooserEntryId_ == 0) return;
        youtubeChooserVisible_ = true;
    } else {
        youtubeChooserVisible_ = false;
        youtubeChooserEntryId_ = 0;
        pendingYoutubeGrab_ = false;
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetYoutubeDownloadKind(youtube::YoutubeDownloadKind kind) {
    if (!youtubeChooserVisible_) return;
    youtubeDownloadSelection_.kind = kind;
    if (youtubeView_.probe && youtubeView_.probeEntryId == youtubeChooserEntryId_) {
        EnsureYoutubeSelection(youtubeDownloadSelection_, *youtubeView_.probe);
    }
    PersistYoutubeChooserSelection();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::CycleYoutubeVideoFormat(int direction) {
    if (!youtubeChooserVisible_ || !youtubeView_.probe ||
        youtubeView_.probeEntryId != youtubeChooserEntryId_) return;
    EnsureYoutubeSelection(youtubeDownloadSelection_, *youtubeView_.probe);
    std::vector<std::wstring> extensions;
    for (const auto& format : youtubeView_.probe->videoFormats) {
        if (std::find(extensions.begin(), extensions.end(), format.extension) == extensions.end()) {
            extensions.push_back(format.extension);
        }
    }
    if (extensions.empty()) return;
    const auto currentFormat = std::find_if(
        youtubeView_.probe->videoFormats.begin(), youtubeView_.probe->videoFormats.end(),
        [this](const auto& format) {
            return format.formatId == youtubeDownloadSelection_.videoFormatId;
        });
    const std::wstring currentExtension = currentFormat == youtubeView_.probe->videoFormats.end()
                                              ? extensions.front()
                                              : currentFormat->extension;
    const auto current = std::find(extensions.begin(), extensions.end(), currentExtension);
    std::size_t index = current == extensions.end() ? 0 : static_cast<std::size_t>(current - extensions.begin());
    const auto count = static_cast<long long>(extensions.size());
    long long next = (static_cast<long long>(index) + static_cast<long long>(direction)) % count;
    if (next < 0) next += count;
    const auto& extension = extensions[static_cast<std::size_t>(next)];
    const auto matching = std::find_if(
        youtubeView_.probe->videoFormats.begin(), youtubeView_.probe->videoFormats.end(),
        [&extension, this](const auto& format) {
            return format.extension == extension &&
                   format.height == youtubeDownloadSelection_.videoHeight &&
                   format.fps == youtubeDownloadSelection_.videoFps;
        });
    const auto fallback = std::find_if(
        youtubeView_.probe->videoFormats.begin(), youtubeView_.probe->videoFormats.end(),
        [&extension](const auto& format) { return format.extension == extension; });
    const auto& selected = matching != youtubeView_.probe->videoFormats.end() ? *matching : *fallback;
    youtubeDownloadSelection_.videoFormatId = selected.formatId;
    youtubeDownloadSelection_.videoHeight = selected.height;
    youtubeDownloadSelection_.videoFps = selected.fps;
    youtubeDownloadSelection_.preferredVideoExtension = selected.extension;
    PersistYoutubeChooserSelection();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::CycleYoutubeVideoQuality(int direction) {
    if (!youtubeChooserVisible_ || !youtubeView_.probe ||
        youtubeView_.probe->videoFormats.empty() ||
        youtubeView_.probeEntryId != youtubeChooserEntryId_) return;
    EnsureYoutubeSelection(youtubeDownloadSelection_, *youtubeView_.probe);
    std::vector<int> heights;
    for (const auto& format : youtubeView_.probe->videoFormats) {
        if (std::find(heights.begin(), heights.end(), format.height) == heights.end()) {
            heights.push_back(format.height);
        }
    }
    const auto current = std::find(heights.begin(), heights.end(), youtubeDownloadSelection_.videoHeight);
    std::size_t index = current == heights.end() ? 0 : static_cast<std::size_t>(current - heights.begin());
    const auto count = static_cast<long long>(heights.size());
    long long next = (static_cast<long long>(index) + static_cast<long long>(direction)) % count;
    if (next < 0) next += count;
    const int height = heights[static_cast<std::size_t>(next)];
    const auto matching = std::find_if(
        youtubeView_.probe->videoFormats.begin(), youtubeView_.probe->videoFormats.end(),
        [height, this](const auto& format) {
            return format.height == height && format.fps == youtubeDownloadSelection_.videoFps;
        });
    const auto fallback = std::find_if(
        youtubeView_.probe->videoFormats.begin(), youtubeView_.probe->videoFormats.end(),
        [height](const auto& format) { return format.height == height; });
    const auto& selected = matching != youtubeView_.probe->videoFormats.end() ? *matching : *fallback;
    youtubeDownloadSelection_.videoFormatId = selected.formatId;
    youtubeDownloadSelection_.videoHeight = selected.height;
    youtubeDownloadSelection_.videoFps = selected.fps;
    youtubeDownloadSelection_.preferredVideoExtension = selected.extension;
    PersistYoutubeChooserSelection();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::CycleYoutubeVideoFps(int direction) {
    if (!youtubeChooserVisible_ || !youtubeView_.probe ||
        youtubeView_.probe->videoFormats.empty() ||
        youtubeView_.probeEntryId != youtubeChooserEntryId_) return;
    EnsureYoutubeSelection(youtubeDownloadSelection_, *youtubeView_.probe);
    std::vector<double> fpsValues;
    for (const auto& format : youtubeView_.probe->videoFormats) {
        if (format.fps <= 0.0 ||
            std::find(fpsValues.begin(), fpsValues.end(), format.fps) != fpsValues.end()) {
            continue;
        }
        fpsValues.push_back(format.fps);
    }
    if (fpsValues.empty()) return;
    const auto current = std::find(fpsValues.begin(), fpsValues.end(), youtubeDownloadSelection_.videoFps);
    std::size_t index = current == fpsValues.end() ? 0 : static_cast<std::size_t>(current - fpsValues.begin());
    const auto count = static_cast<long long>(fpsValues.size());
    long long next = (static_cast<long long>(index) + static_cast<long long>(direction)) % count;
    if (next < 0) next += count;
    const double fps = fpsValues[static_cast<std::size_t>(next)];
    const auto sameHeightAndExtension = std::find_if(
        youtubeView_.probe->videoFormats.begin(), youtubeView_.probe->videoFormats.end(),
        [this, fps](const auto& format) {
            return format.height == youtubeDownloadSelection_.videoHeight &&
                   format.extension == youtubeDownloadSelection_.preferredVideoExtension &&
                   format.fps == fps;
        });
    const auto sameHeight = std::find_if(
        youtubeView_.probe->videoFormats.begin(), youtubeView_.probe->videoFormats.end(),
        [this, fps](const auto& format) {
            return format.height == youtubeDownloadSelection_.videoHeight && format.fps == fps;
        });
    const auto sameExtension = std::find_if(
        youtubeView_.probe->videoFormats.begin(), youtubeView_.probe->videoFormats.end(),
        [this, fps](const auto& format) {
            return format.extension == youtubeDownloadSelection_.preferredVideoExtension &&
                   format.fps == fps;
        });
    const auto matching = sameHeightAndExtension != youtubeView_.probe->videoFormats.end()
                              ? sameHeightAndExtension
                              : (sameHeight != youtubeView_.probe->videoFormats.end()
                                     ? sameHeight
                                     : (sameExtension != youtubeView_.probe->videoFormats.end()
                                            ? sameExtension
                                            : std::find_if(
                                                  youtubeView_.probe->videoFormats.begin(),
                                                  youtubeView_.probe->videoFormats.end(),
                                                  [fps](const auto& format) {
                                                      return format.fps == fps;
                                                  })));
    if (matching == youtubeView_.probe->videoFormats.end()) return;
    youtubeDownloadSelection_.videoFormatId = matching->formatId;
    youtubeDownloadSelection_.videoHeight = matching->height;
    youtubeDownloadSelection_.videoFps = matching->fps;
    youtubeDownloadSelection_.preferredVideoExtension = matching->extension;
    PersistYoutubeChooserSelection();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::CycleYoutubeAudioFormat(int direction) {
    if (!youtubeChooserVisible_ || !youtubeView_.probe ||
        youtubeView_.probeEntryId != youtubeChooserEntryId_) return;
    CycleFormatId(youtubeDownloadSelection_.audioFormatId,
                  youtubeView_.probe->audioFormats, direction);
    if (const auto selected = std::find_if(
            youtubeView_.probe->audioFormats.begin(), youtubeView_.probe->audioFormats.end(),
            [this](const auto& format) {
                return format.formatId == youtubeDownloadSelection_.audioFormatId;
            });
        selected != youtubeView_.probe->audioFormats.end()) {
        youtubeDownloadSelection_.preferredAudioExtension = selected->extension;
        youtubeDownloadSelection_.preferredAudioBitrate = static_cast<int>(std::lround(selected->abr));
    }
    PersistYoutubeChooserSelection();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::CycleYoutubeAudioOutput(int direction) {
    if (!youtubeChooserVisible_) return;
    constexpr int count = 6;
    int current = static_cast<int>(youtubeDownloadSelection_.audioOutput);
    current = (current + direction) % count;
    if (current < 0) current += count;
    youtubeDownloadSelection_.audioOutput =
        static_cast<youtube::YoutubeAudioOutputFormat>(current);
    PersistYoutubeChooserSelection();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetYoutubeAudioQuality(int quality) {
    if (!youtubeChooserVisible_) return;
    youtubeDownloadSelection_.audioQuality = std::clamp(quality, 0, 9);
    PersistYoutubeChooserSelection();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::ConfirmYoutubeDownload() {
    if (!YoutubeFeatureOn() || !youtubeChooserVisible_ || youtubeChooserEntryId_ == 0 ||
        !youtubeView_.probe || youtubeView_.probeEntryId != youtubeChooserEntryId_) return;
    EnsureYoutubeSelection(youtubeDownloadSelection_, *youtubeView_.probe);
    const bool canDownload = youtubeDownloadSelection_.kind == youtube::YoutubeDownloadKind::Video
                                 ? !youtubeView_.probe->videoFormats.empty() &&
                                       !youtubeView_.probe->audioFormats.empty()
                                 : !youtubeView_.probe->audioFormats.empty();
    if (!canDownload) return;
    const auto entryId = youtubeChooserEntryId_;
    youtube_.Download(entryId, settings_.Settings().musicRoot, youtubeDownloadSelection_);
    PersistYoutubeChooserSelection();
    youtubeChooserVisible_ = false;
    youtubeChooserEntryId_ = 0;
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
                                  : L"Install yt-dlp in Preferences → Online";
    } else {
        youtubeView_.status =
            std::to_wstring(youtubeView_.entries.size()) + L" downloaded track(s)";
    }
    ++youtubeView_.generation;
}

void App::OnYoutubeServiceUpdated() {
    if (!YoutubeFeatureOn()) return;
    ApplyYoutubeSnapshot();
    if (!pendingYoutubeGrab_ || youtubeView_.busy ||
        youtubeView_.job != youtube::YoutubeJobKind::Idle) {
        return;
    }
    pendingYoutubeGrab_ = false;
    if (!youtubeView_.entries.empty()) ActivateYoutubeResult(youtubeView_.entries.front().id);
}

void App::ApplyYoutubeSnapshot() {
    const auto previousGen = youtubeView_.generation;
    const bool wasBusy = youtubeView_.busy;
    const auto previousJob = youtubeView_.job;
    youtubeView_ = youtube_.Snapshot();
    if (youtubeChooserVisible_ && youtubeView_.probe &&
        youtubeView_.probeEntryId == youtubeChooserEntryId_) {
        EnsureYoutubeSelection(youtubeDownloadSelection_, *youtubeView_.probe);
    }
    if (youtubeView_.generation == previousGen) return;

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

    ++revision_;
    if (window_) window_->Refresh();
}

} // namespace rivan
