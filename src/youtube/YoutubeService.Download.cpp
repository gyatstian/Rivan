// YoutubeService.Download.cpp
#include "YoutubeService.Internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <system_error>
#include <utility>

namespace rivan::youtube::detail {

std::optional<std::filesystem::path> FindDownloadedFile(
    const std::filesystem::path& directory, std::wstring_view videoId) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return std::nullopt;
    const std::wstring id(videoId);
    const std::wstring needle = L"[" + id + L"]";
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto name = entry.path().filename().wstring();
        const auto stem = entry.path().stem().wstring();
        const auto ext = Lower(entry.path().extension().wstring());
        const bool media = ext == L".mp3" || ext == L".m4a" || ext == L".opus" ||
                           ext == L".webm" || ext == L".wav" || ext == L".flac" ||
                           ext == L".ogg" || ext == L".mp4" || ext == L".m4v";
        if (!media) continue;
        if (stem == id || name.find(needle) != std::wstring::npos) {
            return entry.path();
        }
    }
    return std::nullopt;
}

void RemovePartialDownloads(const std::filesystem::path& directory, std::wstring_view videoId) {
    // yt-dlp writes partial streams as "<title> [<videoId>].<ext>.part" (with .f<NNN>
    // segments under --concurrent-fragments) plus "<title> [<videoId>].<ext>.ytdl"
    // bookkeeping. Only files carrying this job's [<videoId>] marker and a partial
    // name are removed, so completed media and unrelated files are never swept.
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return;
    const std::wstring id(videoId);
    const std::wstring needle = L"[" + id + L"]";
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto name = entry.path().filename().wstring();
        if (name.find(needle) == std::wstring::npos) continue;
        const auto extension = Lower(entry.path().extension().wstring());
        if (extension != L".part" && extension != L".ytdl") {
            continue;
        }
        std::filesystem::remove(entry.path(), ec);
        ec.clear();
    }
}

} // namespace rivan::youtube::detail

namespace rivan::youtube {

void YoutubeService::RunDownload(std::stop_token stop, std::uint64_t entryId,
                                 std::filesystem::path musicRoot,
                                 YoutubeDownloadSelection selection) {
    const bool isVideo = selection.kind == YoutubeDownloadKind::Video;
    const bool convertAudio = !isVideo &&
                              selection.audioOutput != YoutubeAudioOutputFormat::Native;
    YoutubeEntry target;
    bool missing = false;
    std::optional<YoutubeProbe> probe;
    {
        std::scoped_lock lock(mutex_);
        const auto found = std::find_if(state_.entries.begin(), state_.entries.end(),
                                        [entryId](const YoutubeEntry& e) { return e.id == entryId; });
        if (found == state_.entries.end()) {
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            ++state_.generation;
            missing = true;
        } else {
            target = *found;
            if (state_.probe && state_.probeEntryId == entryId) probe = state_.probe;
        }
    }
    if (missing) {
        Notify();
        return;
    }

    const auto ytDlp = LocateYtDlp();
    if (!ytDlp) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"yt-dlp not installed — use Preferences → Online";
            WriteToolFlagsLocked();
            for (auto& entry : state_.entries) {
                if (entry.id == entryId) {
                    entry.downloading = false;
                    entry.failed = true;
                    entry.downloadProgress = -1.0F;
                }
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    if ((convertAudio || isVideo) && !LocateFfmpeg()) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = isVideo ? L"ffmpeg required to merge video and audio — install in Preferences → Online"
                                    : L"ffmpeg required for audio conversion — install in Preferences → Online";
            WriteToolFlagsLocked();
            for (auto& entry : state_.entries) {
                if (entry.id == entryId) {
                    entry.downloading = false;
                    entry.failed = true;
                    entry.downloadProgress = -1.0F;
                }
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    std::error_code ec;
    const auto directory = DownloadDirectory(musicRoot);
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"Unable to create Youtube folder";
            for (auto& entry : state_.entries) {
                if (entry.id == entryId) {
                    entry.downloading = false;
                    entry.failed = true;
                    entry.downloadProgress = -1.0F;
                }
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    const std::wstring outputTemplate =
        (directory / L"%(title)s [%(id)s].%(ext)s").wstring();
    const std::wstring url =
        target.videoId.empty()
            ? (target.webpageUrl.empty() ? std::wstring{} : target.webpageUrl)
            : (L"https://www.youtube.com/watch?v=" + target.videoId);
    if (url.empty()) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"Download failed: missing video id";
            for (auto& entry : state_.entries) {
                if (entry.id == entryId) {
                    entry.downloading = false;
                    entry.failed = true;
                    entry.downloadProgress = -1.0F;
                }
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    if (probe) {
        const auto hasVideo = [&](std::wstring_view id) {
            return std::any_of(probe->videoFormats.begin(), probe->videoFormats.end(),
                               [id](const auto& format) { return format.formatId == id; });
        };
        const auto hasAudio = [&](std::wstring_view id) {
            return std::any_of(probe->audioFormats.begin(), probe->audioFormats.end(),
                               [id](const auto& format) { return format.formatId == id; });
        };
        if (isVideo) {
            if (!hasVideo(selection.videoFormatId) && !probe->videoFormats.empty()) {
                selection.videoFormatId = probe->videoFormats.front().formatId;
            }
            if (!hasAudio(selection.audioFormatId) && !probe->audioFormats.empty()) {
                selection.audioFormatId = probe->audioFormats.front().formatId;
            }
        } else if (!hasAudio(selection.audioFormatId) && !probe->audioFormats.empty()) {
            selection.audioFormatId = probe->audioFormats.front().formatId;
        }
    }

    if (selection.audioQuality < 0) selection.audioQuality = 0;
    if (selection.audioQuality > 9) selection.audioQuality = 9;

    const auto audioFormatName = [&]() -> std::wstring {
        switch (selection.audioOutput) {
        case YoutubeAudioOutputFormat::Mp3: return L"mp3";
        case YoutubeAudioOutputFormat::Aac: return L"aac";
        case YoutubeAudioOutputFormat::Opus: return L"opus";
        case YoutubeAudioOutputFormat::Flac: return L"flac";
        case YoutubeAudioOutputFormat::Wav: return L"wav";
        case YoutubeAudioOutputFormat::Native: break;
        }
        return {};
    };
    const std::wstring selectedAudio = selection.audioFormatId.empty()
                                           ? L"bestaudio"
                                           : selection.audioFormatId;
    std::wstring videoFormat;
    if (selection.videoFormatId.empty()) {
        videoFormat = L"bestvideo+" + selectedAudio;
        } else if (selection.audioFormatId.empty()) {
        videoFormat = selection.videoFormatId + L"+bestaudio";
    } else {
        videoFormat = selection.videoFormatId + L"+" + selection.audioFormatId;
    }
    const std::wstring embedArt =
        convertAudio ? L" --embed-thumbnail --add-metadata --convert-thumbnails jpg"
                     : (isVideo ? L" --embed-thumbnail --add-metadata" : L"");
    std::wstring arguments;
    if (isVideo) {
        arguments = L"--ignore-config --no-cache-dir -f " + detail::QuoteArg(videoFormat) +
                    L" --merge-output-format mp4 --concurrent-fragments 4 --newline "
                    L"--progress --no-warnings --no-playlist" + embedArt +
                    detail::FfmpegLocationArg() + L" -o " + detail::QuoteArg(outputTemplate) +
                    L" " + detail::QuoteArg(url);
    } else if (convertAudio) {
        arguments = L"--ignore-config --no-cache-dir -f " + detail::QuoteArg(selectedAudio) +
                    L" -x --audio-format " + audioFormatName() +
                    L" --audio-quality " + std::to_wstring(selection.audioQuality) +
                    L" --concurrent-fragments 4 --newline --progress --no-warnings "
                    L"--no-playlist" + embedArt + detail::FfmpegLocationArg() + L" -o " +
                    detail::QuoteArg(outputTemplate) + L" " + detail::QuoteArg(url);
    } else {
        arguments = L"--ignore-config --no-cache-dir -f " + detail::QuoteArg(selectedAudio) +
                    L" --concurrent-fragments 4 --newline --progress --no-warnings "
                    L"--no-playlist" + embedArt + L" -o " + detail::QuoteArg(outputTemplate) +
                    L" " + detail::QuoteArg(url);
    }

    auto lastNotify = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    float lastReported = -1.0F;
    bool converting = false;
    const auto pushProgress = [&](float percent, bool force, bool isConvert) {
        const auto now = std::chrono::steady_clock::now();
        const bool enoughTime = force || (now - lastNotify) >= std::chrono::milliseconds(150);
        const bool enoughDelta = force || lastReported < 0.0F ||
                                 std::fabs(percent - lastReported) >= 0.5F ||
                                 isConvert != converting;
        if (!enoughTime && !enoughDelta) return;
        lastNotify = now;
        lastReported = percent;
        converting = isConvert;
        {
            std::scoped_lock lock(mutex_);
            for (auto& entry : state_.entries) {
                if (entry.id != entryId) continue;
                entry.downloadProgress = percent;
                entry.downloading = true;
            }
            if (isConvert) {
                state_.status = isVideo ? L"Merging..." : L"Converting...";
            } else {
                const int shown = static_cast<int>(percent + 0.5F);
                state_.status = L"Downloading " + std::to_wstring(shown) + L"%";
            }
            ++state_.generation;
        }
        Notify();
    };

    std::string output;
    std::string error;
    DWORD exitCode = 1;
    const bool ran = detail::RunProcessCapture(
        *ytDlp, arguments, stop, output, error, &exitCode,
        [&](std::string_view line) {
            if (stop.stop_requested()) return;
            float percent = 0.0F;
            if (detail::ParseDownloadPercent(line, percent)) {
                if (percent > 99.0F) percent = 99.0F;
                pushProgress(percent, false, false);
            } else if (detail::LineLooksLikePostprocess(line)) {
                pushProgress(99.0F, true, true);
            }
        });

    std::optional<std::filesystem::path> local;
    if (!stop.stop_requested()) {
        // A non-zero exit from a ran process is a genuine failure; skip the file
        // lookup so the state update below takes the failure branch.
        if (ran && exitCode != 0) {
            local = std::nullopt;
        } else {
            local = detail::FindDownloadedFile(directory, target.videoId);
        }
    } else {
        // Cancel path: yt-dlp is killed mid-flight by RunProcessCapture, so its
        // partial stream files (*.part, *.ytdl) are left behind. Remove only files
        // matching this job's [<videoId>] marker; downloads are serialized through
        // JoinWorker, so no other job writes the same pattern concurrently.
        detail::RemovePartialDownloads(directory, target.videoId);
    }

    {
        std::scoped_lock lock(mutex_);
        state_.busy = false;
        state_.job = YoutubeJobKind::Idle;
        for (auto& entry : state_.entries) {
            if (entry.id != entryId) continue;
            entry.downloading = false;
            if (local) {
                entry.localPath = *local;
                entry.failed = false;
                entry.downloadProgress = 100.0F;
                state_.status = L"Downloaded: " + local->filename().wstring();
            } else {
                entry.failed = true;
                entry.downloadProgress = -1.0F;
                if (stop.stop_requested()) {
                    state_.status = L"Cancelled";
                } else {
                    const auto errorDetail = detail::TailWide(output.empty() ? error : output, 140);
                    state_.status = errorDetail.empty()
                                        ? L"Download failed"
                                        : (L"Download failed: " + errorDetail);
                }
            }
        }
        ++state_.generation;
    }
    Notify();
}

} // namespace rivan::youtube
