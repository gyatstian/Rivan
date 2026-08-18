// YoutubeService.Probe.cpp
#include "YoutubeService.Internal.h"

#include "../core/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rivan::youtube::detail {
namespace {

const core::JsonValue* Member(const core::JsonValue& object, std::string_view name) {
    return core::JsonMember(object, name);
}

std::string StringMember(const core::JsonValue& object, std::string_view name) {
    const auto* value = Member(object, name);
    return value && value->kind == core::JsonValue::Kind::String ? value->string : std::string{};
}

std::wstring VideoIdMember(const core::JsonValue& object) {
    return Utf8ToWide(StringMember(object, "id"));
}

double NumberMember(const core::JsonValue& object, std::string_view name) {
    const auto* value = Member(object, name);
    return value && value->kind == core::JsonValue::Kind::Number && std::isfinite(value->number)
               ? std::max(0.0, value->number)
               : 0.0;
}

std::uint64_t SizeMember(const core::JsonValue& object) {
    const double exact = NumberMember(object, "filesize");
    const double approximate = NumberMember(object, "filesize_approx");
    const double value = exact > 0.0 ? exact : approximate;
    return value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
               ? std::numeric_limits<std::uint64_t>::max()
               : static_cast<std::uint64_t>(value);
}

// External-process output may append trailing text after the JSON document (e.g. a
// yt-dlp warning line on stdout). Strict ParseJson needs the whole span to be valid, so
// this tolerance is restored for such output only. Returns the candidate's first
// balanced root-object span, or the full candidate when the braces never balance.
[[nodiscard]] std::string_view FirstObjectSpan(std::string_view candidate) noexcept {
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = 0; index < candidate.size(); ++index) {
        const char value = candidate[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') quoted = false;
            continue;
        }
        if (value == '"') {
            quoted = true;
        } else if (value == '{') {
            ++depth;
        } else if (value == '}' && --depth == 0) {
            return candidate.substr(0, index + 1U);
        }
    }
    return candidate;
}

} // namespace

std::optional<YoutubeProbe> ParseProbeJson(const std::string& stdoutText) {
    for (std::size_t start = stdoutText.find('{'); start != std::string::npos;
         start = stdoutText.find('{', start + 1)) {
        const auto candidate = std::string_view(stdoutText).substr(start);
        // The shared parser is intentionally strict (full-input consumption), but yt-dlp
        // may append a trailing warning line; retry the balanced root-object span so
        // external-process output keeps the former trailing-garbage tolerance.
        auto root = core::ParseJson(candidate);
        if (!root) root = core::ParseJson(FirstObjectSpan(candidate));
        if (!root || root->kind != core::JsonValue::Kind::Object) continue;
        const auto title = StringMember(*root, "title");
        const auto* formats = Member(*root, "formats");
        if (title.empty() || !formats || formats->kind != core::JsonValue::Kind::Array) continue;

        YoutubeProbe probe;
        probe.videoId = VideoIdMember(*root);
        probe.title = Utf8ToWide(title);
        probe.durationSeconds = NumberMember(*root, "duration");
        for (const auto& format : formats->array) {
            if (format.kind != core::JsonValue::Kind::Object) continue;
            const auto id = StringMember(format, "format_id");
            const auto extension = StringMember(format, "ext");
            if (id.empty() || extension.empty()) continue;
            const auto videoCodec = StringMember(format, "vcodec");
            const auto audioCodec = StringMember(format, "acodec");
            const double heightValue = NumberMember(format, "height");
            const int height = heightValue > std::numeric_limits<int>::max()
                                   ? 0
                                   : static_cast<int>(heightValue);
            const double fps = NumberMember(format, "fps");
            const double abr = NumberMember(format, "abr");
            const auto filesize = SizeMember(format);
            if (videoCodec != "none" && audioCodec == "none" && height > 0) {
                probe.videoFormats.push_back(
                    YoutubeVideoFormat{Utf8ToWide(id), Utf8ToWide(extension), height, fps, filesize});
            }
            if (audioCodec != "none" && videoCodec == "none") {
                probe.audioFormats.push_back(
                    YoutubeAudioFormat{Utf8ToWide(id), Utf8ToWide(extension), abr, filesize});
            }
        }

        std::sort(probe.videoFormats.begin(), probe.videoFormats.end(), [](const auto& left,
                                                                            const auto& right) {
            if (left.height != right.height) return left.height > right.height;
            if (left.fps != right.fps) return left.fps > right.fps;
            if (left.filesize != right.filesize) return left.filesize > right.filesize;
            return left.formatId < right.formatId;
        });
        std::sort(probe.audioFormats.begin(), probe.audioFormats.end(), [](const auto& left,
                                                                            const auto& right) {
            if (left.abr != right.abr) return left.abr > right.abr;
            if (left.filesize != right.filesize) return left.filesize > right.filesize;
            return left.formatId < right.formatId;
        });
        if (probe.videoFormats.empty() && probe.audioFormats.empty()) continue;
        return probe;
    }
    return std::nullopt;
}

} // namespace rivan::youtube::detail

namespace rivan::youtube {

void YoutubeService::RunProbe(std::stop_token stop, std::uint64_t entryId) {
    YoutubeEntry target;
    bool foundEntry = false;
    {
        std::scoped_lock lock(mutex_);
        const auto found = std::find_if(
            state_.entries.begin(), state_.entries.end(),
            [entryId](const YoutubeEntry& entry) { return entry.id == entryId; });
        if (found != state_.entries.end()) {
            target = *found;
            foundEntry = true;
        } else {
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"Probe failed: entry no longer exists";
            ++state_.generation;
        }
    }
    if (!foundEntry) {
        Notify();
        return;
    }

    const auto ytDlp = LocateYtDlp();
    const std::wstring url = target.videoId.empty()
                                 ? target.webpageUrl
                                 : L"https://www.youtube.com/watch?v=" + target.videoId;
    std::string output;
    std::string error;
    DWORD exitCode = 1;
    std::optional<YoutubeProbe> result;
    if (ytDlp && !url.empty()) {
        if (!LocateDeno()) {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status =
                L"Deno (JS runtime) required for YouTube formats — install in Settings → Online";
            WriteToolFlagsLocked();
            for (auto& entry : state_.entries) {
                if (entry.id != entryId) continue;
                entry.downloading = false;
                entry.failed = true;
                entry.downloadProgress = -1.0F;
            }
            ++state_.generation;
            Notify();
            return;
        }
        // web_embedded is the player client that (with deno present) resolves URL-signed
        // media streams; the android_vr default now returns HTTP 403 on those streams.
        const auto arguments =
            L"--ignore-config --no-cache-dir --extractor-args "
            L"\"youtube:player_client=web_embedded\" --dump-single-json --no-warnings "
            L"--no-playlist " +
            detail::QuoteArg(url);
        if (detail::RunProcessCapture(*ytDlp, arguments, stop, output, error, &exitCode) &&
            exitCode == 0 && !stop.stop_requested()) {
            result = detail::ParseProbeJson(output);
        }
    }

    {
        std::scoped_lock lock(mutex_);
        state_.busy = false;
        state_.job = YoutubeJobKind::Idle;
        if (stop.stop_requested()) {
            state_.status = L"Cancelled";
        } else if (!ytDlp) {
            state_.status = L"yt-dlp not installed — use Settings → Online";
        } else if (result) {
            state_.probe = std::move(result);
            state_.probeEntryId = entryId;
            for (auto& entry : state_.entries) {
                if (entry.id != entryId) continue;
                if (!state_.probe->videoId.empty()) entry.videoId = state_.probe->videoId;
                entry.title = state_.probe->title;
                entry.durationSeconds = state_.probe->durationSeconds;
            }
            state_.status = L"Probe ready";
        } else {
            const auto detailText = detail::TailWide(output.empty() ? error : output, 140);
            state_.status = detailText.empty() ? L"Probe failed"
                                                : L"Probe failed: " + detailText;
        }
        ++state_.generation;
    }
    Notify();
}

} // namespace rivan::youtube
