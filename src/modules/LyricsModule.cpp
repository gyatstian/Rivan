#include "../ui/Win32UiImpl.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace rivan::ui {
namespace {

[[nodiscard]] DWRITE_TEXT_ALIGNMENT LyricsAlignmentToDw(
    config::LyricsTextAlignment alignment) noexcept {
    switch (alignment) {
    case config::LyricsTextAlignment::Center: return DWRITE_TEXT_ALIGNMENT_CENTER;
    case config::LyricsTextAlignment::Right: return DWRITE_TEXT_ALIGNMENT_TRAILING;
    case config::LyricsTextAlignment::Left: break;
    }
    return DWRITE_TEXT_ALIGNMENT_LEADING;
}

// Resolves the caret character (UTF-16 code-unit position) at a point given in the
// verse layout's own coordinate space.
[[nodiscard]] std::uint32_t LyricsHitCharacter(
    const Microsoft::WRL::ComPtr<IDWriteTextLayout>& layout, float localX,
    float localY) noexcept {
    if (!layout) return 0;
    BOOL isTrailing = FALSE;
    BOOL isInside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestPoint(localX, localY, &isTrailing, &isInside, &metrics))) {
        return 0;
    }
    return static_cast<std::uint32_t>(metrics.textPosition +
                                      (isTrailing ? metrics.length : 0U));
}

void SetClipboardText(HWND window, std::wstring_view text) {
    if (text.empty() || !OpenClipboard(window)) return;
    EmptyClipboard();
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* destination = GlobalLock(memory);
        if (destination) {
            std::memcpy(destination, text.data(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr;
        }
        if (memory) GlobalFree(memory);
    }
    CloseClipboard();
}

} // namespace

void Win32Ui::Impl::DrawLyrics(const D2D1_RECT_F& bounds,
                               std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    // The whole module is a scroll target; scrolling must not depend on a narrow strip
    // inside the text bevel (see Scroll).
    lyricsModuleBounds = bounds;
    const auto content = DrawPanel(bounds, UiModuleRegistry::Get(ModuleId::Lyrics).Title(),
                                   b[1].Get(), b[2].Get(), b[3].Get(), b[4].Get(),
                                   b[13].Get(), b[7].Get(), ModuleId::Lyrics);
    if (Width(content) < 20.0F || Height(content) < 45.0F) return;

    const float buttonHeight = 22.0F;
    const float gap = 4.0F;
    const bool syncedAvailable = model.lyrics.available && model.lyrics.document.synced;
    const bool synced = syncedAvailable && lyricsSyncedMode_;
    // A single sync/plain toggle is shown only when both modes exist. With plain-only
    // lyrics there is no mode switch, so the module shows no buttons at all.
    float controlsHeight = 0.0F;
    if (syncedAvailable) {
        const float buttonWidth = std::max(1.0F, Width(content) - gap * 2.0F);
        const auto toggle = Rect(content.left + gap, content.top + gap,
                                 content.left + gap + buttonWidth,
                                 content.top + gap + buttonHeight);
        const bool hot = Contains(toggle, static_cast<float>(mouse.x),
                                  static_cast<float>(mouse.y));
        DrawBevel(toggle, synced ? b[11].Get() : (hot ? b[7].Get() : b[2].Get()),
                  b[3].Get(), b[4].Get(), synced);
        DrawText(synced ? L"UNSYNCED LYRICS" : L"SYNC LYRICS", toggle, b[9].Get(),
                 tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        AddIdHit(toggle, HitKind::LyricsAction, synced ? 2U : 1U);
        controlsHeight = buttonHeight + gap;
    }

    lyricsContentBounds = Rect(content.left + 5.0F, content.top + controlsHeight + 5.0F,
                               content.right - 5.0F, content.bottom - 5.0F);
    DrawBevel(lyricsContentBounds, b[5].Get(), b[3].Get(), b[4].Get(), true, 1.0F);
    const auto& lines = model.lyrics.document.lines;
    if (model.lyrics.revision != lyricsRevision_) {
        lyricsRevision_ = model.lyrics.revision;
        lyricsScrollY = 0.0F;
        lyricsSyncedMode_ = true;
        lyricsUserScrolling_ = false;
        lyricsSelection_.active = false;
    }
    if (model.lyrics.loading || !model.lyrics.available || lines.empty()) {
        const auto status = model.lyrics.status.empty()
                                ? (model.lyrics.loading ? L"LOADING LYRICS..." : L"NO LYRICS AVAILABLE")
                                : model.lyrics.status;
        // Only offer authoring lyrics for an active track that finished loading.
        const bool canAdd = !model.lyrics.loading && model.lyrics.trackId != 0;
        constexpr float addLabelHeight = 26.0F;
        constexpr float addButtonHeight = 22.0F;
        constexpr float addSpacing = 6.0F;
        float top = lyricsContentBounds.top;
        if (canAdd) {
            top = lyricsContentBounds.top +
                  std::max(0.0F, (Height(lyricsContentBounds) - (addLabelHeight + addSpacing + addButtonHeight)) * 0.5F);
        }
        const auto label = Rect(lyricsContentBounds.left, top, lyricsContentBounds.right,
                                top + addLabelHeight);
        DrawText(status, label, b[10].Get(), smallFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        if (canAdd) {
            const float buttonTop = top + addLabelHeight + addSpacing;
            const float buttonBottom = std::min(buttonTop + addButtonHeight, lyricsContentBounds.bottom - 1.0F);
            if (buttonBottom - buttonTop >= 14.0F) {
                const float left = lyricsContentBounds.left + 6.0F;
                const float right = lyricsContentBounds.right - 6.0F;
                const auto button = Rect(left, buttonTop, right, buttonBottom);
                const bool hot = Contains(button, static_cast<float>(mouse.x),
                                          static_cast<float>(mouse.y));
                DrawBevel(button, hot ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(),
                          false);
                DrawText(L"ADD YOUR OWN LYRICS", button, b[9].Get(), tinyFormat.Get(),
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                AddIdHit(button, HitKind::LyricsAction, 3U);
            }
        }
        return;
    }

    const DWRITE_TEXT_ALIGNMENT alignment = LyricsAlignmentToDw(model.lyricsAlignment);
    const float textWidth = std::max(1.0F, Width(lyricsContentBounds) - 12.0F);
    const int widthKey = static_cast<int>(textWidth);
    auto& layouts = lyricsLayouts;
    auto& lineHeights = lyricsLineHeights;
    auto& lineTops = lyricsLineTops;
    float contentHeight = 0.0F;
    if (model.lyrics.revision != lyricsLayoutRevision ||
        widthKey != lyricsLayoutWidthKey ||
        alignment != lyricsLayoutAlignment ||
        layouts.size() != lines.size()) {
        lyricsLayoutRevision = model.lyrics.revision;
        lyricsLayoutWidthKey = widthKey;
        lyricsLayoutAlignment = alignment;
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
            layout->SetTextAlignment(alignment);
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
        if (top + lineHeights[i] >= lyricsContentBounds.top && top <= lyricsContentBounds.bottom) {
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
        if (lyricsSelection_.active && !synced) {
            std::uint32_t selBegin = 0;
            std::uint32_t selEnd = 0;
            if (LyricsSelectionRange(i, selBegin, selEnd)) {
                const UINT32 selLength = selEnd - selBegin;
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(
                    std::max<std::size_t>(1, static_cast<std::size_t>(selLength)));
                UINT32 actual = 0;
                if (SUCCEEDED(layouts[i]->HitTestTextRange(selBegin, selLength, origin.x, origin.y,
                                                           metrics.data(),
                                                           static_cast<UINT32>(metrics.size()),
                                                           &actual))) {
                    for (UINT32 m = 0; m < actual; ++m) {
                        target->FillRectangle(Rect(metrics[m].left, metrics[m].top,
                                                   metrics[m].left + metrics[m].width,
                                                   metrics[m].top + metrics[m].height),
                                              b[11].Get());
                    }
                }
            }
        }
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
        lyricsSelection_.active = false;
        break;
    case 2:
        lyricsSyncedMode_ = false;
        // The synced auto-follow can leave the view pinned at the bottom or mid-document
        // (e.g. near the end of the song), which makes the plain text look frozen and
        // unscrollable. A plain-only song starts at the top and scrolls freely, so restart
        // the plain view at the beginning of the text on switch.
        lyricsScrollY = 0.0F;
        lyricsUserScrolling_ = false;
        lyricsSelection_.active = false;
        break;
    case 3:
        // No lyrics available: let the user author a lyrics file and open it in their
        // text editor. The service picks the file up on the next request for the song.
        host.AddYourOwnLyrics();
        break;
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

bool Win32Ui::Impl::IsLyricsPlainView() const noexcept {
    return !(model.lyrics.available && model.lyrics.document.synced && lyricsSyncedMode_);
}

void Win32Ui::Impl::BeginLyricsTextSelection(std::size_t index, float x, float y) {
    const auto& lines = model.lyrics.document.lines;
    if (index >= lines.size()) return;
    std::uint32_t character = 0;
    if (index < lyricsLayouts.size() && lyricsLayouts[index]) {
        const float localX = x - (lyricsContentBounds.left + 6.0F);
        const float localY = y - (lyricsContentBounds.top + 2.0F - lyricsScrollY +
                                  lyricsLineTops[index]);
        character = LyricsHitCharacter(lyricsLayouts[index], localX, localY);
    }
    lyricsSelection_ = {index, character, index, character, true};
    lyricsSelecting_ = true;
    SetCapture(window);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::UpdateLyricsTextSelection(float x, float y) {
    const auto& lines = model.lyrics.document.lines;
    if (!lyricsSelecting_ || lines.empty()) return;
    std::size_t index = 0;
    if (lyricsLineTops.size() == lines.size() && lyricsLineHeights.size() == lines.size()) {
        const float firstTop = lyricsContentBounds.top + 2.0F - lyricsScrollY;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const float top = firstTop + lyricsLineTops[i];
            if (y < top) { index = i; break; }
            index = i;
        }
    }
    std::uint32_t character = 0;
    if (index < lyricsLayouts.size() && lyricsLayouts[index]) {
        const float localX = x - (lyricsContentBounds.left + 6.0F);
        const float localY = y - (lyricsContentBounds.top + 2.0F - lyricsScrollY +
                                  lyricsLineTops[index]);
        character = LyricsHitCharacter(lyricsLayouts[index], localX, localY);
    }
    lyricsSelection_.caretLine = index;
    lyricsSelection_.caretChar = character;
    InvalidateRect(window, nullptr, FALSE);
}

bool Win32Ui::Impl::LyricsSelectionRange(std::size_t line, std::uint32_t& begin,
                                         std::uint32_t& end) const noexcept {
    if (!lyricsSelection_.active) return false;
    const auto& lines = model.lyrics.document.lines;
    if (line >= lines.size()) return false;
    const std::size_t anchorLine = lyricsSelection_.anchorLine;
    const std::size_t caretLine = lyricsSelection_.caretLine;
    const std::uint32_t anchorChar = lyricsSelection_.anchorChar;
    const std::uint32_t caretChar = lyricsSelection_.caretChar;
    const bool anchorFirst = anchorLine < caretLine ||
                             (anchorLine == caretLine && anchorChar <= caretChar);
    const std::size_t startLine = anchorFirst ? anchorLine : caretLine;
    const std::size_t endLine = anchorFirst ? caretLine : anchorLine;
    if (line < startLine || line > endLine) return false;
    const std::uint32_t size = static_cast<std::uint32_t>(lines[line].text.size());
    begin = 0;
    end = size;
    if (line == startLine && line == endLine) {
        begin = std::min(anchorChar, caretChar);
        end = std::max(anchorChar, caretChar);
    } else if (line == startLine) {
        begin = anchorFirst ? anchorChar : caretChar;
    } else if (line == endLine) {
        end = anchorFirst ? caretChar : anchorChar;
    }
    return begin < end;
}

void Win32Ui::Impl::CopyLyricsSelection() {
    const auto& lines = model.lyrics.document.lines;
    if (!lyricsSelection_.active || lines.empty()) return;
    std::wstring text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        if (!LyricsSelectionRange(i, begin, end)) continue;
        if (!text.empty()) text += L'\n';
        text.append(lines[i].text, static_cast<std::size_t>(begin),
                    static_cast<std::size_t>(end - begin));
    }
    SetClipboardText(window, text);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::SelectAllLyricsText() {
    const auto& lines = model.lyrics.document.lines;
    if (lines.empty()) {
        lyricsSelection_.active = false;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    lyricsSelection_ = {0, 0, lines.size() - 1,
                        static_cast<std::uint32_t>(lines.back().text.size()), true};
    InvalidateRect(window, nullptr, FALSE);
}

} // namespace rivan::ui
