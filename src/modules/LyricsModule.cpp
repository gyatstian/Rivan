#include "../ui/Win32UiImpl.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace rivan::ui {

void Win32Ui::Impl::DrawLyrics(const D2D1_RECT_F& bounds,
                               std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const auto content = DrawPanel(bounds, UiModuleRegistry::Get(ModuleId::Lyrics).Title(),
                                   b[1].Get(), b[2].Get(), b[3].Get(), b[4].Get(),
                                   b[13].Get(), b[7].Get(), ModuleId::Lyrics);
    if (Width(content) < 20.0F || Height(content) < 45.0F) return;

    const float buttonHeight = 22.0F;
    const float gap = 4.0F;
    const std::size_t buttonColumns = Width(content) >= 330.0F ? 6U : 3U;
    const float buttonWidth = (Width(content) - gap * static_cast<float>(buttonColumns + 1U)) /
                              static_cast<float>(buttonColumns);
    const auto button = [this, &b, buttonHeight, gap, buttonWidth, buttonColumns, &content]
        (std::size_t index, std::wstring_view label, std::uint64_t action, bool active) {
            const std::size_t row = index / buttonColumns;
            const std::size_t column = index % buttonColumns;
            const float left = content.left + gap + static_cast<float>(column) * (buttonWidth + gap);
            const float top = content.top + gap + static_cast<float>(row) * (buttonHeight + gap);
            const auto bounds = Rect(left, top, left + buttonWidth, top + buttonHeight);
            const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            DrawBevel(bounds, active ? b[11].Get() : (hot ? b[7].Get() : b[2].Get()),
                      b[3].Get(), b[4].Get(), active);
            DrawText(label, bounds, b[9].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            AddIdHit(bounds, HitKind::LyricsAction, action);
        };
    const bool syncedAvailable = model.lyrics.available && model.lyrics.document.synced;
    const bool synced = syncedAvailable && lyricsSyncedMode_;
    button(0, synced ? L"SYNC" : L"PLAIN", 1, synced);
    button(1, L"TEXT", 2, !synced);
    button(2, L"LEFT", 3, lyricsAlignment_ == DWRITE_TEXT_ALIGNMENT_LEADING);
    button(3, L"CENTER", 4, lyricsAlignment_ == DWRITE_TEXT_ALIGNMENT_CENTER);
    button(4, L"RIGHT", 5, lyricsAlignment_ == DWRITE_TEXT_ALIGNMENT_TRAILING);
    button(5, L"COPY", 6, false);

    const float controlsHeight = static_cast<float>((6U + buttonColumns - 1U) / buttonColumns) *
                                 (buttonHeight + gap);
    lyricsContentBounds = Rect(content.left + 5.0F, content.top + controlsHeight + 5.0F,
                               content.right - 5.0F, content.bottom - 5.0F);
    DrawBevel(lyricsContentBounds, b[5].Get(), b[3].Get(), b[4].Get(), true, 1.0F);
    const auto& lines = model.lyrics.document.lines;
    if (model.lyrics.revision != lyricsRevision_) {
        lyricsRevision_ = model.lyrics.revision;
        lyricsScrollY = 0.0F;
        lyricsSyncedMode_ = true;
        lyricsUserScrolling_ = false;
    }
    if (model.lyrics.loading || !model.lyrics.available || lines.empty()) {
        const auto status = model.lyrics.status.empty()
                                ? (model.lyrics.loading ? L"LOADING LYRICS..." : L"NO LYRICS AVAILABLE")
                                : model.lyrics.status;
        DrawText(status, lyricsContentBounds, b[10].Get(), smallFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    const float textWidth = std::max(1.0F, Width(lyricsContentBounds) - 12.0F);
    const int widthKey = static_cast<int>(textWidth);
    auto& layouts = lyricsLayouts;
    auto& lineHeights = lyricsLineHeights;
    auto& lineTops = lyricsLineTops;
    float contentHeight = 0.0F;
    if (model.lyrics.revision != lyricsLayoutRevision ||
        widthKey != lyricsLayoutWidthKey ||
        lyricsAlignment_ != lyricsLayoutAlignment ||
        layouts.size() != lines.size()) {
        lyricsLayoutRevision = model.lyrics.revision;
        lyricsLayoutWidthKey = widthKey;
        lyricsLayoutAlignment = lyricsAlignment_;
        layouts.clear();
        layouts.reserve(lines.size());
        lineHeights.assign(lines.size(), 20.0F);
        lineTops.assign(lines.size(), 0.0F);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(writeFactory->CreateTextLayout(lines[i].text.data(),
                                                       static_cast<UINT32>(lines[i].text.size()),
                                                       regularFormat.Get(), textWidth, 10000.0F,
                                                       layout.ReleaseAndGetAddressOf()))) {
                layouts.push_back(nullptr);
                continue;
            }
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            layout->SetTextAlignment(lyricsAlignment_);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                lineHeights[i] = std::max(20.0F, std::ceil(metrics.height));
            }
            lineTops[i] = contentHeight;
            contentHeight += lineHeights[i];
            layouts.push_back(std::move(layout));
        }
        lyricsContentHeight = contentHeight;
    } else {
        contentHeight = lyricsContentHeight;
        lineHeights = lyricsLineHeights;
        lineTops = lyricsLineTops;
    }

    std::size_t activeLine = 0;
    if (synced) {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].timestampSeconds >= 0.0 &&
                lines[i].timestampSeconds <= model.positionSeconds) activeLine = i;
        }
        // Manual scrolling holds the view; auto-follow resumes once the user scrolls
        // back to the bottom (lyricsUserScrolling_ cleared by Scroll) or re-enables SYNC.
        if (!lyricsUserScrolling_) {
            const float viewportHeight = Height(lyricsContentBounds) - 4.0F;
            const float desired = std::max(0.0F, lineTops[activeLine] - viewportHeight * 0.42F);
            const float maxScroll = std::max(0.0F, contentHeight - viewportHeight);
            lyricsScrollY = std::clamp(desired, 0.0F, maxScroll);
        }
    }
    target->PushAxisAlignedClip(lyricsContentBounds, D2D1_ANTIALIAS_MODE_ALIASED);
    const float firstTop = lyricsContentBounds.top + 2.0F - lyricsScrollY;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float top = firstTop + lineTops[i];
        if (synced && lines[i].timestampSeconds >= 0.0 &&
            top + lineHeights[i] >= lyricsContentBounds.top && top <= lyricsContentBounds.bottom) {
            AddIdHit(Rect(lyricsContentBounds.left, top, lyricsContentBounds.right,
                          top + lineHeights[i]),
                     HitKind::LyricsVerse, static_cast<std::uint64_t>(i));
        }
        if (!layouts[i] || top + lineHeights[i] < lyricsContentBounds.top || top > lyricsContentBounds.bottom) continue;
        const bool active = synced && i == activeLine;
        if (active) {
            // Highlight the current lyric with the skin's panel background color.
            target->FillRectangle(Rect(lyricsContentBounds.left + 4.0F, top,
                                       lyricsContentBounds.right - 4.0F, top + lineHeights[i]),
                                  b[1].Get());
        }
        const auto origin = D2D1::Point2F(lyricsContentBounds.left + 6.0F, top);
        ID2D1Brush* brush = active ? b[12].Get() : b[9].Get();
        if (deferTexts) {
            // Copy (AddRef) into the deferred list: the cached layout must survive the
            // flush so subsequent frames can reuse it without rebuilding.
            deferredTextLayouts.push_back({layouts[i], origin, brush});
        } else {
            target->DrawTextLayout(origin, layouts[i].Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    target->PopAxisAlignedClip();
}

void Win32Ui::Impl::HandleLyricsAction(std::uint64_t action) {
    switch (action) {
    case 1:
        lyricsSyncedMode_ = true;
        lyricsUserScrolling_ = false;
        break;
    case 2:
        lyricsSyncedMode_ = false;
        // The synced auto-follow can leave the view pinned at the bottom or mid-document
        // (e.g. near the end of the song), which makes the plain text look frozen and
        // unscrollable. A plain-only song starts at the top and scrolls freely, so restart
        // the plain view at the beginning of the text on switch.
        lyricsScrollY = 0.0F;
        lyricsUserScrolling_ = false;
        break;
    case 3:
        lyricsAlignment_ = DWRITE_TEXT_ALIGNMENT_LEADING;
        break;
    case 4:
        lyricsAlignment_ = DWRITE_TEXT_ALIGNMENT_CENTER;
        break;
    case 5:
        lyricsAlignment_ = DWRITE_TEXT_ALIGNMENT_TRAILING;
        break;
    case 6: {
        const auto text = model.lyrics.document.PlainText();
        if (text.empty() || !OpenClipboard(window)) break;
        EmptyClipboard();
        const auto bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* destination = GlobalLock(memory);
            if (destination) {
                std::memcpy(destination, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr;
            }
            if (memory) GlobalFree(memory);
        }
        CloseClipboard();
        break;
    }
    default:
        break;
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::HandleLyricsVerse(std::size_t index) {
    const auto& lines = model.lyrics.document.lines;
    if (!lyricsSyncedMode_ || !model.lyrics.document.synced || index >= lines.size()) return;
    const double timestamp = lines[index].timestampSeconds;
    if (!std::isfinite(timestamp) || timestamp < 0.0 ||
        !std::isfinite(model.durationSeconds) || model.durationSeconds <= 0.0) {
        return;
    }
    try {
        host.Seek(std::clamp(timestamp / model.durationSeconds, 0.0, 1.0));
    } catch (...) {}
    // Jumping to a verse implies returning to synced following.
    lyricsUserScrolling_ = false;
    InvalidateRect(window, nullptr, FALSE);
}

} // namespace rivan::ui
