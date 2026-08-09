// Win32Ui.YoutubeChooser.cpp
// Direct2D format chooser for a pending YouTube download.
#include "Win32UiImpl.h"

#include <cmath>

namespace rivan::ui {
namespace {

constexpr std::uint64_t kModeVideo = 1;
constexpr std::uint64_t kModeAudio = 2;
constexpr std::uint64_t kVideoPrevious = 10;
constexpr std::uint64_t kVideoNext = 11;
constexpr std::uint64_t kVideoQualityPrevious = 12;
constexpr std::uint64_t kVideoQualityNext = 13;
constexpr std::uint64_t kVideoFpsPrevious = 14;
constexpr std::uint64_t kVideoFpsNext = 15;
constexpr std::uint64_t kAudioPrevious = 20;
constexpr std::uint64_t kAudioNext = 21;
constexpr std::uint64_t kOutputPrevious = 30;
constexpr std::uint64_t kOutputNext = 31;
constexpr std::uint64_t kQualityPrevious = 40;
constexpr std::uint64_t kQualityNext = 41;
constexpr std::uint64_t kConfirm = 50;
constexpr std::uint64_t kCancel = 51;

[[nodiscard]] std::wstring FormatBytes(std::uint64_t bytes) {
    if (bytes == 0) return L"unknown";
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < 4) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buffer[32]{};
    if (unit == 0) {
        swprintf_s(buffer, L"%llu %s", static_cast<unsigned long long>(bytes), units[unit]);
    } else if (value >= 100.0) {
        swprintf_s(buffer, L"%.0f %s", value, units[unit]);
    } else if (value >= 10.0) {
        swprintf_s(buffer, L"%.1f %s", value, units[unit]);
    } else {
        swprintf_s(buffer, L"%.2f %s", value, units[unit]);
    }
    return buffer;
}

[[nodiscard]] std::uint64_t AddSizes(std::uint64_t left, std::uint64_t right) {
    if (left == 0 || right == 0 || UINT64_MAX - left < right) return 0;
    return left + right;
}

[[nodiscard]] std::wstring VideoLabel(const youtube::YoutubeVideoFormat& format) {
    std::wstring label = format.extension;
    std::transform(label.begin(), label.end(), label.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towupper(character)); });
    return label;
}

[[nodiscard]] std::wstring AudioLabel(const youtube::YoutubeAudioFormat& format) {
    std::wstring label = format.abr > 0.0 ? std::to_wstring(static_cast<int>(std::lround(format.abr))) + L" kbps"
                                          : L"bitrate unknown";
    label += L" / " + format.extension;
    return label;
}

[[nodiscard]] const wchar_t* OutputLabel(youtube::YoutubeAudioOutputFormat format) noexcept {
    switch (format) {
    case youtube::YoutubeAudioOutputFormat::Native: return L"NATIVE";
    case youtube::YoutubeAudioOutputFormat::Mp3: return L"MP3";
    case youtube::YoutubeAudioOutputFormat::Aac: return L"AAC";
    case youtube::YoutubeAudioOutputFormat::Opus: return L"OPUS";
    case youtube::YoutubeAudioOutputFormat::Flac: return L"FLAC";
    case youtube::YoutubeAudioOutputFormat::Wav: return L"WAV";
    }
    return L"NATIVE";
}

[[nodiscard]] const wchar_t* QualityText(youtube::YoutubeAudioOutputFormat format,
                                          int quality) noexcept {
    if (format == youtube::YoutubeAudioOutputFormat::Native) return L"Native stream quality";
    if (format == youtube::YoutubeAudioOutputFormat::Flac) return L"Lossless conversion";
    if (format == youtube::YoutubeAudioOutputFormat::Wav) return L"Uncompressed PCM";
    switch (quality) {
    case 0: return L"Best quality";
    case 1: return L"Very high quality";
    case 2: return L"High quality";
    case 3: return L"Good quality";
    case 4: return L"Balanced quality";
    case 5: return L"Medium quality";
    case 6: return L"Compact quality";
    case 7: return L"Low quality";
    case 8: return L"Very low quality";
    default: return L"Smallest file";
    }
}

} // namespace

void Win32Ui::Impl::DrawYoutubeChooser(
    const D2D1_SIZE_F size, std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    hits.clear();
    target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());

    const auto panel = Rect(12.0F, 12.0F, size.width - 12.0F, size.height - 12.0F);
    const auto content = DrawPanel(panel, L"YOUTUBE DOWNLOAD", b[1].Get(), b[2].Get(),
                                   b[3].Get(), b[4].Get(), b[13].Get(), b[7].Get());
    const float left = content.left + 14.0F;
    const float right = content.right - 14.0F;
    const float buttonHeight = 30.0F;
    const float gap = 8.0F;

    const auto chooserButton = [this, &b, buttonHeight](const D2D1_RECT_F& bounds,
                                                          const std::wstring& label,
                                                          std::uint64_t action, bool active = false,
                                                          bool enabled = true) {
        const bool hot = enabled && Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        DrawBevel(bounds, hot || active ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(), active);
        DrawText(label, bounds, enabled ? (active ? b[12].Get() : b[9].Get()) : b[10].Get(), smallFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        if (enabled) AddIdHit(bounds, HitKind::YoutubeChooserAction, action);
        (void)buttonHeight;
    };
    const auto cycleButton = [this, &b](const D2D1_RECT_F& bounds, const wchar_t* label,
                                        std::uint64_t action, bool enabled = true) {
        const bool hot = enabled && Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        DrawBevel(bounds, hot ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get());
        DrawText(label, bounds, enabled ? b[9].Get() : b[10].Get(), tinyFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        if (enabled) AddIdHit(bounds, HitKind::YoutubeChooserAction, action);
    };
    const auto rowLabel = [this, &b, left, right](const wchar_t* label, float y) {
        DrawText(label, Rect(left, y, right, y + 16.0F), b[8].Get(), tinyFormat.Get());
    };

    DrawText(model.youtubeProbe
                 ? (model.youtubeProbe->title.empty() ? L"Untitled video" : model.youtubeProbe->title)
                 : L"Preparing download options...",
             Rect(left, content.top + 2, right, content.top + 28), b[12].Get(), headingFormat.Get());
    DrawText(model.youtubeStatus.empty() ? L" " : model.youtubeStatus,
             Rect(left, content.top + 29, right, content.top + 48),
             model.youtubeBusy ? b[8].Get() : b[10].Get(), tinyFormat.Get());

    const auto modeTop = content.top + 58.0F;
    const float modeWidth = std::min(190.0F, (right - left - gap) * 0.5F);
    chooserButton(Rect(left, modeTop, left + modeWidth, modeTop + buttonHeight),
                  L"VIDEO + AUDIO", kModeVideo,
                  model.youtubeDownloadSelection.kind == youtube::YoutubeDownloadKind::Video);
    chooserButton(Rect(left + modeWidth + gap, modeTop, left + modeWidth * 2.0F + gap,
                       modeTop + buttonHeight), L"AUDIO ONLY", kModeAudio,
                  model.youtubeDownloadSelection.kind == youtube::YoutubeDownloadKind::AudioOnly);

    const auto& selection = model.youtubeDownloadSelection;
    const auto* probe = model.youtubeProbe ? &*model.youtubeProbe : nullptr;
    const bool ready = probe != nullptr && model.youtubeChooserEntryId != 0;
    float y = modeTop + buttonHeight + 18.0F;
    if (!ready) {
        DrawText(probe ? L"No usable media formats were reported."
                       : (model.youtubeBusy ? L"Loading available formats..."
                                            : L"Unable to load available formats."),
                  Rect(left, y, right, y + 40.0F), b[10].Get(), regularFormat.Get());
    } else if (selection.kind == youtube::YoutubeDownloadKind::Video) {
        rowLabel(L"VIDEO FORMAT", y);
        y += 17.0F;
        const float arrows = 34.0F;
        const auto videoValue = Rect(left, y, right - arrows * 2.0F - gap * 2.0F, y + buttonHeight);
        const auto videoPrev = Rect(videoValue.right + gap, y, videoValue.right + gap + arrows, y + buttonHeight);
        const auto videoNext = Rect(videoPrev.right + gap, y, right, y + buttonHeight);
        std::wstring videoText = L"No video stream";
        if (!probe->videoFormats.empty()) {
            const auto found = std::find_if(probe->videoFormats.begin(), probe->videoFormats.end(),
                                            [&selection](const auto& f) { return f.formatId == selection.videoFormatId; });
            videoText = VideoLabel(found == probe->videoFormats.end() ? probe->videoFormats.front() : *found);
        }
        cycleButton(videoValue, videoText.c_str(), kVideoNext, !probe->videoFormats.empty());
        cycleButton(videoPrev, L"<", kVideoPrevious, !probe->videoFormats.empty());
        cycleButton(videoNext, L">", kVideoNext, !probe->videoFormats.empty());
        y += buttonHeight + 13.0F;

        rowLabel(L"VIDEO QUALITY/FPS", y);
        y += 17.0F;
        const auto qualityValue = Rect(left, y, right - arrows * 2.0F - gap * 2.0F, y + buttonHeight);
        const auto qualityPrev = Rect(qualityValue.right + gap, y, qualityValue.right + gap + arrows, y + buttonHeight);
        const auto qualityNext = Rect(qualityPrev.right + gap, y, right, y + buttonHeight);
        const auto qualityText = selection.videoHeight > 0
            ? std::to_wstring(selection.videoHeight) + L"p"
            : L"unknown";
        cycleButton(qualityValue, qualityText.c_str(), kVideoQualityNext, !probe->videoFormats.empty());
        cycleButton(qualityPrev, L"<", kVideoQualityPrevious, !probe->videoFormats.empty());
        cycleButton(qualityNext, L">", kVideoQualityNext, !probe->videoFormats.empty());
        y += buttonHeight + 13.0F;

        rowLabel(L"FPS", y);
        y += 17.0F;
        const auto fpsValue = Rect(left, y, right - arrows * 2.0F - gap * 2.0F, y + buttonHeight);
        const auto fpsPrev = Rect(fpsValue.right + gap, y, fpsValue.right + gap + arrows, y + buttonHeight);
        const auto fpsNext = Rect(fpsPrev.right + gap, y, right, y + buttonHeight);
        const bool canCycleFps = selection.videoFps > 0.0 && std::any_of(
            probe->videoFormats.begin(), probe->videoFormats.end(), [&selection](const auto& format) {
                return format.fps > 0.0 && format.fps != selection.videoFps;
            });
        const auto fpsText = selection.videoFps > 0.0
            ? std::to_wstring(static_cast<int>(std::lround(selection.videoFps))) +
                  (canCycleFps ? L" FPS" : L" FPS (source limit)")
            : L"unknown";
        cycleButton(fpsValue, fpsText.c_str(), kVideoFpsNext, canCycleFps);
        cycleButton(fpsPrev, L"<", kVideoFpsPrevious, canCycleFps);
        cycleButton(fpsNext, L">", kVideoFpsNext, canCycleFps);
        y += buttonHeight + 13.0F;

        rowLabel(L"AUDIO SOURCE", y);
        y += 17.0F;
        const auto audioValue = Rect(left, y, right - arrows * 2.0F - gap * 2.0F, y + buttonHeight);
        const auto audioPrev = Rect(audioValue.right + gap, y, audioValue.right + gap + arrows, y + buttonHeight);
        const auto audioNext = Rect(audioPrev.right + gap, y, right, y + buttonHeight);
        std::wstring audioText = L"No audio stream";
        std::uint64_t audioSize = 0;
        if (!probe->audioFormats.empty()) {
            const auto found = std::find_if(probe->audioFormats.begin(), probe->audioFormats.end(),
                                            [&selection](const auto& f) { return f.formatId == selection.audioFormatId; });
            const auto& format = found == probe->audioFormats.end() ? probe->audioFormats.front() : *found;
            audioText = AudioLabel(format);
            audioSize = format.filesize;
        }
        cycleButton(audioValue, audioText.c_str(), kAudioNext, !probe->audioFormats.empty());
        cycleButton(audioPrev, L"<", kAudioPrevious, !probe->audioFormats.empty());
        cycleButton(audioNext, L">", kAudioNext, !probe->audioFormats.empty());
        y += buttonHeight + 13.0F;

        rowLabel(L"ESTIMATED SIZE", y);
        y += 17.0F;
        std::uint64_t videoSize = 0;
        if (!probe->videoFormats.empty()) {
            const auto found = std::find_if(probe->videoFormats.begin(), probe->videoFormats.end(),
                                            [&selection](const auto& f) { return f.formatId == selection.videoFormatId; });
            videoSize = (found == probe->videoFormats.end() ? probe->videoFormats.front() : *found).filesize;
        }
        DrawText(FormatBytes(AddSizes(videoSize, audioSize)), Rect(left, y, right, y + buttonHeight),
                 b[9].Get(), regularFormat.Get());
    } else {
        rowLabel(L"AUDIO SOURCE", y);
        y += 17.0F;
        const float arrows = 34.0F;
        const auto audioValue = Rect(left, y, right - arrows * 2.0F - gap * 2.0F, y + buttonHeight);
        const auto audioPrev = Rect(audioValue.right + gap, y, audioValue.right + gap + arrows, y + buttonHeight);
        const auto audioNext = Rect(audioPrev.right + gap, y, right, y + buttonHeight);
        std::wstring audioText = L"No audio stream";
        std::uint64_t audioSize = 0;
        if (!probe->audioFormats.empty()) {
            const auto found = std::find_if(probe->audioFormats.begin(), probe->audioFormats.end(),
                                            [&selection](const auto& f) { return f.formatId == selection.audioFormatId; });
            const auto& format = found == probe->audioFormats.end() ? probe->audioFormats.front() : *found;
            audioText = AudioLabel(format);
            audioSize = format.filesize;
        }
        cycleButton(audioValue, audioText.c_str(), kAudioNext, !probe->audioFormats.empty());
        cycleButton(audioPrev, L"<", kAudioPrevious, !probe->audioFormats.empty());
        cycleButton(audioNext, L">", kAudioNext, !probe->audioFormats.empty());
        y += buttonHeight + 13.0F;

        rowLabel(L"AUDIO OUTPUT", y);
        y += 17.0F;
        const float outputArrows = 34.0F;
        const auto outputValue = Rect(left, y, right - outputArrows * 2.0F - gap * 2.0F, y + buttonHeight);
        const auto outputPrev = Rect(outputValue.right + gap, y, outputValue.right + gap + outputArrows, y + buttonHeight);
        const auto outputNext = Rect(outputPrev.right + gap, y, right, y + buttonHeight);
        cycleButton(outputValue, OutputLabel(selection.audioOutput), kOutputNext);
        cycleButton(outputPrev, L"<", kOutputPrevious);
        cycleButton(outputNext, L">", kOutputNext);
        y += buttonHeight + 13.0F;

        rowLabel(L"ENCODER QUALITY", y);
        y += 17.0F;
        const auto qualityValue = Rect(left, y, right - outputArrows * 2.0F - gap * 2.0F, y + buttonHeight);
        const auto qualityPrev = Rect(qualityValue.right + gap, y, qualityValue.right + gap + outputArrows, y + buttonHeight);
        const auto qualityNext = Rect(qualityPrev.right + gap, y, right, y + buttonHeight);
        const auto quality = std::to_wstring(std::clamp(selection.audioQuality, 0, 9)) + L" / 9 - " +
                             QualityText(selection.audioOutput, std::clamp(selection.audioQuality, 0, 9));
        cycleButton(qualityValue, quality.c_str(), kQualityNext);
        cycleButton(qualityPrev, L"<", kQualityPrevious);
        cycleButton(qualityNext, L">", kQualityNext);
        y += buttonHeight + 13.0F;

        rowLabel(L"ESTIMATED SIZE", y);
        y += 17.0F;
        if (selection.audioOutput != youtube::YoutubeAudioOutputFormat::Native && audioSize != 0) {
            audioSize = static_cast<std::uint64_t>(audioSize * (0.45 + (9 - selection.audioQuality) * 0.06));
        }
        DrawText(FormatBytes(audioSize), Rect(left, y, right, y + buttonHeight), b[9].Get(), regularFormat.Get());
    }

    const float bottom = content.bottom - buttonHeight;
    const float half = (right - left - gap) * 0.5F;
    const bool canDownload = ready &&
        (selection.kind == youtube::YoutubeDownloadKind::AudioOnly
             ? !probe->audioFormats.empty()
             : !probe->videoFormats.empty() && !probe->audioFormats.empty());
    chooserButton(Rect(left, bottom, left + half, bottom + buttonHeight), L"DOWNLOAD", kConfirm,
                  false, canDownload);
    chooserButton(Rect(left + half + gap, bottom, right, bottom + buttonHeight), L"CANCEL", kCancel);
}

void Win32Ui::Impl::HandleYoutubeChooserAction(std::uint64_t action) {
    try {
        switch (action) {
        case kModeVideo: host.SetYoutubeDownloadKind(youtube::YoutubeDownloadKind::Video); break;
        case kModeAudio: host.SetYoutubeDownloadKind(youtube::YoutubeDownloadKind::AudioOnly); break;
        case kVideoPrevious: host.CycleYoutubeVideoFormat(-1); break;
        case kVideoNext: host.CycleYoutubeVideoFormat(1); break;
        case kVideoQualityPrevious: host.CycleYoutubeVideoQuality(-1); break;
        case kVideoQualityNext: host.CycleYoutubeVideoQuality(1); break;
        case kVideoFpsPrevious: host.CycleYoutubeVideoFps(-1); break;
        case kVideoFpsNext: host.CycleYoutubeVideoFps(1); break;
        case kAudioPrevious: host.CycleYoutubeAudioFormat(-1); break;
        case kAudioNext: host.CycleYoutubeAudioFormat(1); break;
        case kOutputPrevious: host.CycleYoutubeAudioOutput(-1); break;
        case kOutputNext: host.CycleYoutubeAudioOutput(1); break;
        case kQualityPrevious: host.SetYoutubeAudioQuality(model.youtubeDownloadSelection.audioQuality - 1); break;
        case kQualityNext: host.SetYoutubeAudioQuality(model.youtubeDownloadSelection.audioQuality + 1); break;
        case kConfirm: host.ConfirmYoutubeDownload(); break;
        case kCancel: host.SetYoutubeChooserVisible(false); break;
        default: break;
        }
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

} // namespace rivan::ui
