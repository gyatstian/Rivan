// Win32Ui.Settings.SongRowLayoutEditor.cpp
// Song row layout editor rendering, field drag, and snap handling for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::DrawSongRowLayoutEditor(
    const float left, const float right, float& y,
    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    DrawText(L"SONG ROW LAYOUT", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    if (!songRowEditorVisible) {
        SettingsButton(Rect(left, y, right, y + 24), L"CUSTOMIZE SONG ROWS", 70, b);
        y += 32;
        return;
    }

    DrawText(L"LAYOUT PREVIEW", Rect(left, y, right, y + 16), b[6].Get(), tinyFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 19;
    const float previewHeight = std::max(76.0F, songRowDraft.rowHeight + 26.0F);
    const auto previewFrame = Rect(left, y, right, y + previewHeight);
    DrawBevel(previewFrame, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
    const auto preview = Rect(previewFrame.left + 4.0F,
                              previewFrame.top + (Height(previewFrame) - songRowDraft.rowHeight) * 0.5F,
                              previewFrame.right - 4.0F,
                              previewFrame.top + (Height(previewFrame) - songRowDraft.rowHeight) * 0.5F +
                                  songRowDraft.rowHeight);
    songRowPreviewBounds = preview;
    TrackView sample;
    sample.title = L"Sample song name";
    sample.artist = L"Sample author";
    sample.durationSeconds = 213.0;
    sample.bitrateKbps = 320;
    DrawSongRow(preview, sample, 1, static_cast<std::size_t>(-1), false, b[9].Get(), b[10].Get(),
                b[12].Get(), &songRowDraft, true, &songRowFieldBounds);
    for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
        const auto field = static_cast<SongRowField>(index);
        const auto& bounds = songRowFieldBounds[index];
        if (!songRowDraft.Field(field).visible || Width(bounds) <= 0.0F || Height(bounds) <= 0.0F) continue;
        const bool selected = songRowSelectedField && *songRowSelectedField == field;
        target->DrawRectangle(bounds, selected ? b[12].Get() : b[8].Get(), selected ? 2.0F : 1.0F);
        if (selected) {
            target->FillRectangle(Rect(bounds.right - 5.0F, bounds.bottom - 5.0F,
                                       bounds.right + 1.0F, bounds.bottom + 1.0F), b[12].Get());
        }
        const auto viewport = Rect(settingsDetailsBounds.left + 2.0F,
                                   settingsDetailsBounds.top + 15.0F,
                                   settingsDetailsBounds.right - 2.0F,
                                   settingsDetailsBounds.bottom - 4.0F);
        const auto hitBounds = Rect(std::max(bounds.left, viewport.left),
                                    std::max(bounds.top, viewport.top),
                                    std::min(bounds.right, viewport.right),
                                    std::min(bounds.bottom, viewport.bottom));
        if (Width(hitBounds) > 0.0F && Height(hitBounds) > 0.0F) {
            AddIdHit(hitBounds, HitKind::SongRowField, index);
        }
    }
    if (songRowDragging && songRowDragMoved && songRowSnap) {
        const auto targetIndex = static_cast<std::size_t>(songRowSnap->target);
        if (targetIndex < kSongRowFieldCount && songRowDraft.Field(songRowSnap->target).visible) {
            const auto& targetBounds = songRowFieldBounds[targetIndex];
            constexpr float markerWidth = 3.0F;
            const auto marker = songRowSnap->side == SongRowSnapSide::Left
                ? Rect(targetBounds.left, targetBounds.top,
                       std::min(targetBounds.right, targetBounds.left + markerWidth), targetBounds.bottom)
                : Rect(std::max(targetBounds.left, targetBounds.right - markerWidth), targetBounds.top,
                       targetBounds.right, targetBounds.bottom);
            if (Width(marker) > 0.0F && Height(marker) > 0.0F) {
                target->FillRectangle(marker, b[12].Get());
            }
        }
    }
    if (!songRowDragging && songRowSnap && songRowSelectedField) {
        const auto selectedIndex = static_cast<std::size_t>(*songRowSelectedField);
        const auto targetIndex = static_cast<std::size_t>(songRowSnap->target);
        if (selectedIndex < kSongRowFieldCount && targetIndex < kSongRowFieldCount &&
            songRowDraft.Field(*songRowSelectedField).visible &&
            songRowDraft.Field(songRowSnap->target).visible) {
            const auto& selectedBounds = songRowFieldBounds[selectedIndex];
            const auto& targetBounds = songRowFieldBounds[targetIndex];
            const float boundary = songRowSnap->side == SongRowSnapSide::Left
                ? (selectedBounds.right + targetBounds.left) * 0.5F
                : (targetBounds.right + selectedBounds.left) * 0.5F;
            const float overlapTop = std::max({preview.top, selectedBounds.top, targetBounds.top});
            const float overlapBottom = std::min({preview.bottom, selectedBounds.bottom, targetBounds.bottom});
            const float controlWidth = std::min(18.0F, Width(preview));
            const float controlHeight = std::min(56.0F, Height(preview));
            if (controlWidth > 0.0F && controlHeight > 0.0F) {
                const float buttonHeight = std::min(17.0F, controlHeight / 3.0F);
                const float controlLeft = std::clamp(boundary - controlWidth * 0.5F,
                                                     preview.left, preview.right - controlWidth);
                const float controlTop = std::clamp((overlapTop + overlapBottom - controlHeight) * 0.5F,
                                                    preview.top, preview.bottom - controlHeight);
                const auto control = Rect(controlLeft, controlTop,
                                          controlLeft + controlWidth, controlTop + controlHeight);
                SettingsButton(Rect(control.left, control.top, control.right,
                                    control.top + buttonHeight),
                               L"+", 83, b);
                DrawBevel(Rect(control.left, control.top + buttonHeight,
                               control.right, control.bottom - buttonHeight),
                          b[5].Get(), b[3].Get(), b[4].Get(), true);
                DrawText(std::to_wstring(songRowSnap->gapPixels),
                         Rect(control.left, control.top + buttonHeight,
                              control.right, control.bottom - buttonHeight),
                         b[6].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
                SettingsButton(Rect(control.left, control.bottom - buttonHeight,
                                    control.right, control.bottom),
                               L"-", 84, b);
            }
        }
    }
    y = previewFrame.bottom + 7.0F;

    const auto selectedField = songRowSelectedField;
    if (selectedField) {
        auto& field = songRowDraft.Field(*selectedField);
        DrawText(SongRowFieldName(*selectedField), Rect(left, y, right - 38.0F, y + 20),
                 b[6].Get(), tinyFormat.Get());
        SettingsButton(Rect(right - 34.0F, y, right, y + 22), L"X", 72, b);
        y += 26.0F;
        const float third = (right - left - 12.0F) / 3.0F;
        SettingsButton(Rect(left, y, left + third, y + 22),
                       L"FONT -", 73, b);
        const auto fontValue = Rect(left + third + 6.0F, y, left + 2.0F * third + 6.0F, y + 22);
        DrawBevel(fontValue, b[5].Get(), b[3].Get(), b[4].Get(), true);
        DrawText(L"FONT " + std::to_wstring(field.fontSizeDelta) + L" PX", fontValue,
                 b[6].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        SettingsButton(Rect(left + 2.0F * third + 12.0F, y, right, y + 22),
                       L"FONT +", 74, b);
        y += 26.0F;
        const wchar_t* weight = field.fontWeight == SongRowFontWeight::Bold
            ? L"WEIGHT: BOLD" : field.fontWeight == SongRowFontWeight::SemiBold
                ? L"WEIGHT: SEMIBOLD" : L"WEIGHT: NORMAL";
        SettingsButton(Rect(left, y, left + third * 1.5F + 3.0F, y + 22), weight, 76, b);
        SettingsButton(Rect(left + third * 1.5F + 9.0F, y, right, y + 22),
                       field.textColor == SongRowTextColor::Primary
                           ? L"COLOR: PRIMARY" : L"COLOR: SECONDARY", 77, b);
        y += 27.0F;
        SettingsButton(Rect(left, y, right, y + 22),
                       field.fontStyle == SongRowFontStyle::Italic
                           ? L"STYLE: ITALIC" : L"STYLE: NORMAL", 82, b);
        y += 27.0F;
    } else {
        DrawText(L"FIELD CONTROLS", Rect(left, y, right, y + 18),
                 b[10].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 23.0F;
    }

    DrawText(L"ROW HEIGHT", Rect(left, y, right, y + 16), b[6].Get(), tinyFormat.Get());
    y += 18.0F;
    const float half = (right - left - 6.0F) * 0.5F;
    SettingsButton(Rect(left, y, left + half, y + 22), L"HEIGHT -", 80, b);
    SettingsButton(Rect(left + half + 6.0F, y, right, y + 22), L"HEIGHT +", 81, b);
    y += 28.0F;

    bool hasHiddenField = false;
    for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
        if (!songRowDraft.Field(static_cast<SongRowField>(index)).visible) {
            hasHiddenField = true;
            break;
        }
    }
    if (hasHiddenField) {
        DrawText(L"ADD FIELD", Rect(left, y, right, y + 16), b[6].Get(), tinyFormat.Get());
        y += 18.0F;
        float x = left;
        for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
            const auto field = static_cast<SongRowField>(index);
            if (songRowDraft.Field(field).visible) continue;
            const float width = std::min(150.0F, right - x);
            SettingsButton(Rect(x, y, x + width, y + 22), SongRowFieldName(field), 90 + index, b);
            x += width + 5.0F;
            if (x + 90.0F > right) {
                x = left;
                y += 26.0F;
            }
        }
        y += 27.0F;
    }
    SettingsButton(Rect(left, y, right, y + 22), L"RESET SONG ROW LAYOUT", 71, b);
    y += 30.0F;
}

void Win32Ui::Impl::BeginSongRowFieldDrag(const SongRowField field, const float x, const float y) {
    const auto index = static_cast<std::size_t>(field);
    if (index >= songRowFieldBounds.size() || !songRowDraft.Field(field).visible) return;
    const auto bounds = songRowFieldBounds[index];
    if (Width(bounds) <= 0.0F || Height(bounds) <= 0.0F) return;
    songRowSelectedField = field;
    songRowResizing = x >= bounds.right - 9.0F && y >= bounds.bottom - 9.0F;
    songRowDragMoved = false;
    songRowDragStart = {x, y};
    songRowSnap = songRowDraft.Field(field).snap;
    songRowDragging = true;
    songRowDragOffset = {x - bounds.left, y - bounds.top};
    SetCapture(window);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::UpdateSongRowFieldDrag(const float x, const float y) {
    if (!songRowDragging || !songRowSelectedField || Width(songRowPreviewBounds) <= 0.0F ||
        Height(songRowPreviewBounds) <= 0.0F) return;
    auto& field = songRowDraft.Field(*songRowSelectedField);
    const float canvasWidth = Width(songRowPreviewBounds);
    const float canvasHeight = Height(songRowPreviewBounds);
    if (canvasWidth <= 0.0F || canvasHeight <= 0.0F) return;
    if (!songRowDragMoved && std::hypot(x - songRowDragStart.x, y - songRowDragStart.y) >= 2.0F) {
        songRowDragMoved = true;
        if (!songRowResizing) field.snap.reset();
    }
    if (!songRowDragMoved) {
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    SongRowFieldLayout candidate = field;
    const auto currentBounds = songRowFieldBounds[static_cast<std::size_t>(*songRowSelectedField)];
    const float currentWidth = field.fluid
        ? SongRowFluidFieldWidth(Width(currentBounds), canvasWidth)
        : field.width;
    if (songRowResizing) {
        const float resolvedX = (currentBounds.left - kSongRowFieldHorizontalInsetPixels -
                                 songRowPreviewBounds.left) / canvasWidth;
        candidate.width = std::clamp((x + kSongRowFieldHorizontalInsetPixels -
                                      songRowPreviewBounds.left) / canvasWidth - resolvedX,
                                     0.02F, 1.0F - resolvedX);
        candidate.x = std::clamp(resolvedX, 0.0F, 1.0F - candidate.width);
        candidate.height = std::clamp((y + 1.0F - songRowPreviewBounds.top) / canvasHeight - field.y,
                                      0.02F, 1.0F - field.y);
        candidate.fluid = false;
    } else {
        candidate.x = std::clamp((x - songRowDragOffset.x -
                                  kSongRowFieldHorizontalInsetPixels -
                                  songRowPreviewBounds.left) / canvasWidth,
                                 0.0F, 1.0F - currentWidth);
        candidate.y = std::clamp((y - songRowDragOffset.y - 1.0F -
                                  songRowPreviewBounds.top) / canvasHeight,
                                 0.0F, 1.0F - field.height);
    }
    if (!songRowResizing && songRowDragMoved) {
        std::optional<SongRowSnap> bestSnap;
        float bestDistance = kSongRowSnapDistancePixels;
        for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
            const auto other = static_cast<SongRowField>(index);
            if (other == *songRowSelectedField || !songRowDraft.Field(other).visible) continue;
            const auto otherBounds = songRowFieldBounds[index];
            const auto side = SongRowSnapHoverSide(
                x, y, otherBounds.left, otherBounds.right, otherBounds.top, otherBounds.bottom);
            if (!side) continue;
            const float edge = *side == SongRowSnapSide::Left ? otherBounds.left : otherBounds.right;
            const float distance = std::abs(x - edge);
            if (distance > bestDistance) continue;
            const int gap = songRowSnap && songRowSnap->target == other &&
                    songRowSnap->side == *side
                ? songRowSnap->gapPixels : kSongRowDefaultSnapGapPixels;
            bestDistance = distance;
            bestSnap = SongRowSnap{other, *side, gap};
        }
        if (bestSnap) songRowSnap = bestSnap;
        else songRowSnap.reset();
    }
    field = candidate;
    try { host.PreviewSongRowLayout(songRowDraft); } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

bool Win32Ui::Impl::ApplySongRowSnap() noexcept {
    if (!songRowSnap || !songRowSelectedField || Width(songRowPreviewBounds) <= 0.0F) return false;
    const auto targetIndex = static_cast<std::size_t>(songRowSnap->target);
    if (targetIndex >= kSongRowFieldCount || *songRowSelectedField == songRowSnap->target ||
        !songRowDraft.Field(*songRowSelectedField).visible ||
        !songRowDraft.Field(songRowSnap->target).visible) return false;
    const auto targetBounds = songRowFieldBounds[targetIndex];
    auto& field = songRowDraft.Field(*songRowSelectedField);
    const auto selectedIndex = static_cast<std::size_t>(*songRowSelectedField);
    const float fieldWidth = field.fluid && selectedIndex < songRowFieldBounds.size()
        ? SongRowFluidFieldWidth(Width(songRowFieldBounds[selectedIndex]),
                                 Width(songRowPreviewBounds))
        : field.width;
    field.snap = songRowSnap;
    const float snappedX = std::clamp(
        SongRowSnappedFieldX(targetBounds.left, targetBounds.right,
                             songRowPreviewBounds.left, Width(songRowPreviewBounds), fieldWidth,
                             static_cast<float>(songRowSnap->gapPixels), songRowSnap->side),
        0.0F, 1.0F - fieldWidth);
    const float oldX = field.x;
    field.x = snappedX;
    if (SongRowHasSnapCycle(songRowDraft)) {
        field.snap.reset();
        field.x = oldX;
        return false;
    }
    // Keep the stored fallback geometry valid even when a fluid field is narrower
    // than its persisted width. The renderer uses the measured width at draw time.
    field.x = std::clamp(field.x, 0.0F, 1.0F -
                         (field.fluid ? fieldWidth : field.width));
    return true;
}

void Win32Ui::Impl::AdjustSongRowSnapGap(const int delta) {
    if (!songRowSnap || !songRowSelectedField ||
        songRowDragging ||
        Width(songRowPreviewBounds) <= 0.0F) return;
    const auto targetIndex = static_cast<std::size_t>(songRowSnap->target);
    if (targetIndex >= kSongRowFieldCount ||
        !songRowDraft.Field(*songRowSelectedField).visible ||
        !songRowDraft.Field(songRowSnap->target).visible) return;
    songRowSnap->gapPixels = std::clamp(songRowSnap->gapPixels + delta,
                                        kSongRowMinimumSnapGapPixels,
                                        kSongRowMaximumSnapGapPixels);
    if (ApplySongRowSnap()) CommitSongRowLayoutEdit();
}

void Win32Ui::Impl::CommitSongRowLayoutEdit() {
    try { host.SetSongRowLayout(songRowDraft); } catch (...) {}
}

} // namespace rivan::ui
