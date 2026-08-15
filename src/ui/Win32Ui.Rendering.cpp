// Win32Ui.Rendering.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

[[nodiscard]] const std::vector<const TrackView*>& Win32Ui::Impl::Filtered(const std::vector<TrackView>& source,
                                                                  const std::wstring& query) {
        if (cachedTrackRowsRevision == model.revision && cachedTrackRowsQuery == query) {
            return cachedTrackRows;
        }
        cachedTrackRowsRevision = model.revision;
        cachedTrackRowsQuery = query;
        cachedTrackRows.clear();
        cachedTrackRows.reserve(source.size());
        const std::wstring lowerQuery = Lowercase(query);
        for (const auto& track : source) {
            if (MatchesLowered(track, lowerQuery)) cachedTrackRows.push_back(&track);
        }
        return cachedTrackRows;
    }

// Position of a TrackView (borrowed from model.tracks) within that vector. Selection
    // and drag reorder key off this stable model index, not the filtered row index, so
    // duplicate entries and search filtering stay unambiguous.
[[nodiscard]] std::size_t Win32Ui::Impl::ModelTrackIndex(const TrackView* track) const noexcept {
        if (model.tracks.empty()) return static_cast<std::size_t>(-1);
        const auto* base = model.tracks.data();
        if (track < base || track >= base + model.tracks.size()) {
            return static_cast<std::size_t>(-1);
        }
        return static_cast<std::size_t>(track - base);
    }

[[nodiscard]] std::size_t Win32Ui::Impl::SourceTrackIndex(const TrackView* track) const noexcept {
        const auto modelIndex = ModelTrackIndex(track);
        if (modelIndex == static_cast<std::size_t>(-1)) return modelIndex;
        const auto source = track->sourcePlaylistId;
        if (source == 0) return modelIndex;
        std::size_t sourceIndex = 0;
        for (std::size_t index = 0; index < modelIndex; ++index) {
            if (model.tracks[index].sourcePlaylistId == source) ++sourceIndex;
        }
        return model.tracks[modelIndex].sourcePlaylistId == source
                   ? sourceIndex
                   : static_cast<std::size_t>(-1);
    }

void Win32Ui::Impl::DrawTrackRenameField(const D2D1_RECT_F& bounds, ID2D1Brush* textBrush) {
        Win32Ui::Impl::DrawBevel(bounds, currentBrushes[3], currentBrushes[4], currentBrushes[5], true);
        const auto textBounds = Rect(bounds.left + 4, bounds.top, bounds.right - 4, bounds.bottom);
        if (trackNameSelectAll) target->FillRectangle(textBounds, currentBrushes[11]);
        Win32Ui::Impl::DrawText(trackNameBuffer, textBounds, textBrush, regularFormat.Get());
        if (trackNameSelectAll) return;

        const std::size_t cursor = std::min(trackNameCursor, trackNameBuffer.size());
        float caretX = textBounds.left;
        if (writeFactory && regularFormat && !trackNameBuffer.empty()) {
            regularFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            regularFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(writeFactory->CreateTextLayout(
                    trackNameBuffer.data(), static_cast<UINT32>(trackNameBuffer.size()),
                    regularFormat.Get(), Width(textBounds), Height(textBounds), &layout))) {
                FLOAT x{};
                FLOAT y{};
                DWRITE_HIT_TEST_METRICS metrics{};
                if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(cursor), FALSE,
                                                          &x, &y, &metrics))) {
                    caretX = textBounds.left + x;
                }
            }
        }
        caretX = std::clamp(caretX, textBounds.left, textBounds.right - 1.0F);
        target->DrawLine({caretX, textBounds.top + 3.0F}, {caretX, textBounds.bottom - 3.0F},
                         textBrush, 1.0F);
    }

// Thin horizontal insertion bar drawn between rows while a track drag is active.
void Win32Ui::Impl::DrawTrackDropIndicator(const D2D1_RECT_F& row, bool below, ID2D1Brush* brush) {
        const float y = below ? row.bottom : row.top;
        target->FillRectangle(Rect(row.left + 2, y - 1.0F, row.right - 2, y + 1.0F), brush);
}

[[nodiscard]] float Win32Ui::Impl::TrackRowHeight() const noexcept {
    return std::clamp(model.songRowLayout.rowHeight, 20.0F, 160.0F);
}

[[nodiscard]] IDWriteTextFormat* Win32Ui::Impl::SongRowFormat(
    const int sizeDelta, const SongRowFontWeight weight, const SongRowFontStyle style) {
    const auto key = std::make_tuple(sizeDelta, weight, style);
    if (const auto found = songRowFormats.find(key); found != songRowFormats.end()) {
        return found->second.Get();
    }
    DWRITE_FONT_WEIGHT directWriteWeight = DWRITE_FONT_WEIGHT_NORMAL;
    switch (weight) {
    case SongRowFontWeight::SemiBold: directWriteWeight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
    case SongRowFontWeight::Bold: directWriteWeight = DWRITE_FONT_WEIGHT_BOLD; break;
    case SongRowFontWeight::Normal: break;
    }
    const auto& typography = model.activeSkin.typography;
    std::wstring family(typography.fontFamily.begin(), typography.fontFamily.end());
    if (family.empty()) family = L"Segoe UI";
    std::filesystem::path customFile;
    if (!typography.customFontFile.empty() && !model.activeSkin.directory.empty()) {
        customFile = model.activeSkin.directory / typography.customFontFile;
    }
    const float size = std::clamp(typography.baseSize + static_cast<float>(sizeDelta),
                                  7.0F, 72.0F);
    const DWRITE_FONT_STYLE directWriteStyle = style == SongRowFontStyle::Italic
        ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    auto format = BuildTextFormat(family, size, directWriteWeight, customFile, directWriteStyle);
    auto [inserted, ignored] = songRowFormats.emplace(key, std::move(format));
    (void)ignored;
    return inserted->second ? inserted->second.Get() : regularFormat.Get();
}

void Win32Ui::Impl::DrawSongRow(
    const D2D1_RECT_F& row, const TrackView& track, const std::size_t number,
    const std::size_t modelIndex, const bool selected, ID2D1Brush* primary,
    ID2D1Brush* secondary, ID2D1Brush* active, const SongRowLayout* suppliedLayout,
    const bool sample, std::array<D2D1_RECT_F, kSongRowFieldCount>* fieldBounds) {
    const SongRowLayout& layout = suppliedLayout ? *suppliedLayout : model.songRowLayout;
    const auto canvas = row;
    const bool activeRow = selected || track.playing;
    const bool showIndicator = track.playing && !sample;

    std::array<std::wstring, kSongRowFieldCount> fieldValues{};
    std::array<DWRITE_TEXT_ALIGNMENT, kSongRowFieldCount> fieldAlignments{};
    std::array<float, kSongRowFieldCount> fieldTextAdvances{};
    std::array<float, kSongRowFieldCount> fieldTextLeftOverhangs{};
    std::array<float, kSongRowFieldCount> fieldTextRightOverhangs{};
    std::array<bool, kSongRowFieldCount> measuredTextFields{};
    for (auto& alignment : fieldAlignments) alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    fieldValues[static_cast<std::size_t>(SongRowField::Number)] = std::to_wstring(number) + L".";
    fieldValues[static_cast<std::size_t>(SongRowField::Title)] =
        track.title.empty() ? L"Untitled" : track.title;
    fieldValues[static_cast<std::size_t>(SongRowField::Duration)] = FormatTime(track.durationSeconds);
    fieldValues[static_cast<std::size_t>(SongRowField::Artist)] =
        track.artist.empty() ? (sample ? L"Artist name" : L"") : track.artist;
    fieldValues[static_cast<std::size_t>(SongRowField::Bitrate)] =
        track.bitrateKbps > 0 ? std::to_wstring(track.bitrateKbps) + L" kbps"
                              : (sample ? L"320 kbps" : L"-- kbps");
    const float textOutline = std::clamp(model.activeSkin.typography.borderSize, 0.0F, 8.0F);

    std::array<float, kSongRowFieldCount> fieldWidths{};
    for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
        const auto field = static_cast<SongRowField>(index);
        const auto& fieldLayout = layout.Field(field);
        fieldWidths[index] = fieldLayout.width;
        if (!fieldLayout.visible || field == SongRowField::Cover || !fieldLayout.fluid ||
            Width(canvas) <= 0.0F || !writeFactory) {
            continue;
        }
        ComPtr<IDWriteTextLayout> textLayout;
        if (SUCCEEDED(writeFactory->CreateTextLayout(
                fieldValues[index].data(), static_cast<UINT32>(fieldValues[index].size()),
                SongRowFormat(fieldLayout.fontSizeDelta, fieldLayout.fontWeight, fieldLayout.fontStyle),
                100000.0F, std::max(1.0F, Height(canvas)), &textLayout))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(textLayout->GetMetrics(&metrics))) {
                const float advance = metrics.widthIncludingTrailingWhitespace;
                textLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textLayout->SetMaxWidth(std::max(1.0F, advance));
                DWRITE_OVERHANG_METRICS overhang{};
                if (SUCCEEDED(textLayout->GetOverhangMetrics(&overhang))) {
                    fieldTextLeftOverhangs[index] = std::max(0.0F, overhang.left);
                    fieldTextRightOverhangs[index] = std::max(0.0F, overhang.right);
                }
                fieldTextAdvances[index] = advance;
                measuredTextFields[index] = true;
            }
        }
        // Allocate measured glyph overhang, outline, and a one-pixel raster guard
        // in addition to the renderer's one-pixel inset on each side.
        if (measuredTextFields[index]) {
            fieldWidths[index] = SongRowFluidFieldWidth(
                fieldTextAdvances[index] + fieldTextLeftOverhangs[index] +
                    fieldTextRightOverhangs[index] + 2.0F * textOutline,
                Width(canvas));
        }
    }
    SongRowTransientLayout transient;
    if (showIndicator && Width(canvas) > 0.0F) {
        transient.numberMinimumX = 13.0F / Width(canvas);
    }
    const auto resolvedXs = SongRowResolvedFieldXs(
        layout, canvas.left, Width(canvas), fieldWidths, transient);
    if (showIndicator) {
        DrawText(L">", Rect(row.left + 1.0F, row.top, row.left + 12.0F, row.bottom),
                 primary, SongRowFormat(0, SongRowFontWeight::Bold));
    }
    std::array<D2D1_RECT_F, kSongRowFieldCount> renderBounds{};
    for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
        const auto field = static_cast<SongRowField>(index);
        const auto& fieldLayout = layout.Field(field);
        if (!fieldLayout.visible) continue;
        auto bounds = Rect(canvas.left + resolvedXs[index] * Width(canvas),
                           canvas.top + fieldLayout.y * Height(canvas),
                           canvas.left + (resolvedXs[index] + fieldWidths[index]) * Width(canvas),
                           canvas.top + (fieldLayout.y + fieldLayout.height) * Height(canvas));
        bounds.left += 1.0F;
        bounds.right -= 1.0F;
        bounds.top += 1.0F;
        bounds.bottom -= 1.0F;
        renderBounds[index] = bounds;
        if (fieldBounds) (*fieldBounds)[index] = bounds;
    }
    const auto fieldPriority = [](const SongRowField field) noexcept {
        switch (field) {
        case SongRowField::Number:
        case SongRowField::Cover: return 100;
        case SongRowField::Duration:
        case SongRowField::Bitrate: return 90;
        case SongRowField::Title:
        case SongRowField::Artist: return 50;
        case SongRowField::Count: return 0;
        }
        return 0;
    };
    const auto verticalOverlap = [](const D2D1_RECT_F& first,
                                    const D2D1_RECT_F& second) noexcept {
        return std::min(first.bottom, second.bottom) > std::max(first.top, second.top);
    };
    bool renameDrawn = false;
    for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
        const auto field = static_cast<SongRowField>(index);
        const auto& fieldLayout = layout.Field(field);
        if (!fieldLayout.visible) {
            if (fieldBounds) (*fieldBounds)[index] = {};
            continue;
        }
        const auto bounds = renderBounds[index];
        if (field == SongRowField::Cover) {
            if (sample) {
                DrawBevel(bounds, currentBrushes[2], currentBrushes[3], currentBrushes[4], true);
                DrawText(L"ART", bounds, secondary, tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                DrawTrackCover(track, bounds);
            }
            continue;
        }
        ID2D1Brush* brush = fieldLayout.textColor == SongRowTextColor::Primary ? primary : secondary;
        if (activeRow && !sample) brush = active;
        if (field == SongRowField::Title && trackNameEditing && trackRenameIndex == modelIndex && !sample) {
            DrawTrackRenameField(bounds, active);
            renameDrawn = true;
        } else {
            auto textSlot = bounds;
            const int priority = fieldPriority(field);
            for (std::size_t otherIndex = 0; otherIndex < kSongRowFieldCount; ++otherIndex) {
                const auto other = static_cast<SongRowField>(otherIndex);
                if (other == field || !layout.Field(other).visible ||
                    fieldPriority(other) < priority ||
                    !verticalOverlap(bounds, renderBounds[otherIndex])) continue;
                const auto& otherBounds = renderBounds[otherIndex];
                if (otherBounds.left > bounds.left && otherBounds.left < textSlot.right) {
                    textSlot.right = std::min(textSlot.right,
                                              otherBounds.left - kSongRowDefaultSnapGapPixels);
                }
                if (otherBounds.right > bounds.left && otherBounds.right < bounds.right &&
                    otherBounds.left <= bounds.left) {
                    textSlot.left = std::max(textSlot.left,
                                             otherBounds.right + kSongRowDefaultSnapGapPixels);
                }
            }
            if (textSlot.right < textSlot.left) textSlot.right = textSlot.left;
            auto textBounds = textSlot;
            D2D1_DRAW_TEXT_OPTIONS drawOptions = D2D1_DRAW_TEXT_OPTIONS_CLIP;
            const D2D1_RECT_F* clipBounds = nullptr;
            bool trim = false;
            if (fieldLayout.fluid && measuredTextFields[index]) {
                const float requiredWidth = fieldTextAdvances[index] +
                    fieldTextLeftOverhangs[index] + fieldTextRightOverhangs[index] +
                    2.0F * (textOutline + kSongRowFluidTextSafetyPixels);
                trim = Width(textSlot) + 0.01F < requiredWidth;
                const float leftPadding = kSongRowFluidTextSafetyPixels + textOutline +
                    fieldTextLeftOverhangs[index];
                const float rightPadding = kSongRowFluidTextSafetyPixels + textOutline +
                    fieldTextRightOverhangs[index];
                if (fieldAlignments[index] == DWRITE_TEXT_ALIGNMENT_TRAILING) {
                    textBounds.right = textSlot.right - rightPadding;
                    textBounds.left = textBounds.right - fieldTextAdvances[index];
                } else {
                    textBounds.left = textSlot.left + leftPadding;
                    textBounds.right = textBounds.left + fieldTextAdvances[index];
                }
                // Bounds include the glyph overhang and outline room. Clip only at
                // the field edge, never at the inner text rectangle.
                drawOptions = D2D1_DRAW_TEXT_OPTIONS_NONE;
                clipBounds = &textSlot;
                if (trim) textBounds = textSlot;
            }
            DrawText(fieldValues[index], textBounds, brush,
                     SongRowFormat(fieldLayout.fontSizeDelta, fieldLayout.fontWeight,
                                   fieldLayout.fontStyle), fieldAlignments[index],
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER, drawOptions, clipBounds, trim);
        }
    }
    if (!sample && !renameDrawn && trackNameEditing && trackRenameIndex == modelIndex) {
        DrawTrackRenameField(Rect(canvas.left + 4.0F, canvas.top + 2.0F,
                                  canvas.right - 4.0F, canvas.bottom - 2.0F), active);
    }
}

void Win32Ui::Impl::DrawTrackRows(const D2D1_RECT_F& bounds, const std::vector<const TrackView*>& tracks,
                       std::size_t& scroll, std::size_t& visibleRows,
                       ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* greenDim,
                       ID2D1Brush* selection, ID2D1Brush* white) {
        // SCREEN: Track list, including the < NO MATCHING TRACKS > state.
        if (screen == currentBrushes[5]) screenBounds.push_back(bounds);
        target->FillRectangle(bounds, screen);
        const float rowHeight = TrackRowHeight();
        visibleRows = static_cast<std::size_t>(std::max(0.0F, std::floor(Height(bounds) / rowHeight)));
        const std::size_t maximum = tracks.size() > visibleRows ? tracks.size() - visibleRows : 0;
        scroll = std::min(scroll, maximum);
        const bool dragging = dragActive && dragKind == DragKind::Track;
        target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        for (std::size_t rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
            const std::size_t index = scroll + rowIndex;
            if (index >= tracks.size()) break;
            const auto& track = *tracks[index];
            const std::size_t modelIndex = ModelTrackIndex(tracks[index]);
            const bool selected = trackSelection.contains(modelIndex);
            const float top = bounds.top + static_cast<float>(rowIndex) * rowHeight;
            const auto row = Rect(bounds.left, top, bounds.right, top + rowHeight);
            const bool hot = Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (selected || track.playing || hot) {
                target->FillRectangle(row, selected || track.playing ? selection
                                                                     : currentBrushes[7]);
            }
            DrawSongRow(row, track, index + 1, modelIndex, selected, green, greenDim, white);
            HitRegion hit;
            hit.bounds = row;
            hit.kind = HitKind::Track;
            hit.id = track.id;
            hit.index = modelIndex;
            hits.push_back(hit);
            if (dragging && dropTrackIndex != static_cast<std::size_t>(-1)) {
                const auto sourceIndex = SourceTrackIndex(tracks[index]);
                if (dropTrackIndex == sourceIndex) DrawTrackDropIndicator(row, false, white);
                else if (dropTrackIndex == sourceIndex + 1) DrawTrackDropIndicator(row, true, white);
            }
        }
        target->PopAxisAlignedClip();
        if (tracks.empty()) {
            Win32Ui::Impl::DrawText(L"< NO MATCHING TRACKS >", bounds, greenDim, regularFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        }

// Renders the current folder view: the selected folder's loose tracks first (no
    // header) followed by one separator header per subfolder section. Falls back to a
    // plain flat list when the model provides no sections. Search filters tracks by
    // title/artist/album and hides sections left empty.
    void Win32Ui::Impl::DrawSectionedTracks(const D2D1_RECT_F& bounds, std::size_t& scroll,
                             std::size_t& visibleRows,
                             ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* greenDim,
                             ID2D1Brush* selection, ID2D1Brush* white) {
        if (model.trackSections.empty()) {
            const auto& filtered = Filtered(model.tracks, playlistQuery);
            Win32Ui::Impl::DrawTrackRows(bounds, filtered, scroll, visibleRows, screen, green, greenDim,
                          selection, white);
            return;
        }
        // Flatten into a header/track row stream, honoring the active search filter.
        if (cachedSectionRowsRevision != model.revision || cachedSectionRowsQuery != playlistQuery) {
            cachedSectionRowsRevision = model.revision;
            cachedSectionRowsQuery = playlistQuery;
            cachedSectionRows.clear();
            cachedSectionRows.reserve(model.tracks.size() + model.trackSections.size());
            for (const auto& section : model.trackSections) {
                const std::size_t first = cachedSectionRows.size();
                const std::size_t last = std::min(section.first + section.count, model.tracks.size());
                const std::wstring lowerQuery = Lowercase(playlistQuery);
                for (std::size_t i = section.first; i < last; ++i) {
                    if (MatchesLowered(model.tracks[i], lowerQuery)) {
                        cachedSectionRows.push_back({false, {}, &model.tracks[i]});
                    }
                }
                if (cachedSectionRows.size() != first && !section.label.empty()) {
                    cachedSectionRows.insert(cachedSectionRows.begin() + static_cast<std::ptrdiff_t>(first),
                                             {true, section.label, nullptr});
                }
            }
        }
        const auto& rows = cachedSectionRows;

        if (screen == currentBrushes[5]) screenBounds.push_back(bounds);
        target->FillRectangle(bounds, screen);
        const float rowHeight = TrackRowHeight();
        visibleRows = static_cast<std::size_t>(std::max(0.0F, std::floor(Height(bounds) / rowHeight)));
        const std::size_t maximum = rows.size() > visibleRows ? rows.size() - visibleRows : 0;
        scroll = std::min(scroll, maximum);
        target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        std::size_t trackNumber = 0;
        for (std::size_t prior = 0; prior < scroll; ++prior) {
            if (!rows[prior].header) ++trackNumber;
        }
        for (std::size_t rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
            const std::size_t index = scroll + rowIndex;
            if (index >= rows.size()) break;
            const auto& row = rows[index];
            const float top = bounds.top + static_cast<float>(rowIndex) * rowHeight;
            const auto rect = Rect(bounds.left, top, bounds.right, top + rowHeight);
            if (row.header) {
                const float mid = (rect.top + rect.bottom) * 0.5F;
                target->DrawLine({rect.left + 4, mid}, {rect.left + 20, mid}, greenDim, 1.0F);
                Win32Ui::Impl::DrawText(row.label, Rect(rect.left + 24, rect.top, rect.right - 24, rect.bottom),
                         greenDim, tinyFormat.Get());
                const float textWidth = std::min(Width(rect) * 0.5F, 8.0F * row.label.size());
                target->DrawLine({rect.left + 28 + textWidth, mid}, {rect.right - 4, mid}, greenDim, 1.0F);
                continue;
            }
            const auto& track = *row.track;
            const std::size_t modelIndex = ModelTrackIndex(row.track);
            const bool selected = trackSelection.contains(modelIndex);
            const bool hot = Contains(rect, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (selected || track.playing || hot) {
                target->FillRectangle(rect, selected || track.playing ? selection
                                                                      : currentBrushes[7]);
            }
            DrawSongRow(rect, track, ++trackNumber, modelIndex, selected, green, greenDim, white);
            HitRegion hit;
            hit.bounds = rect;
            hit.kind = HitKind::Track;
            hit.id = track.id;
            hit.index = modelIndex;
            hits.push_back(hit);
            if (dragActive && dragKind == DragKind::Track &&
                dropTrackIndex != static_cast<std::size_t>(-1)) {
                const auto sourceIndex = SourceTrackIndex(row.track);
                if (dropTrackIndex == sourceIndex) DrawTrackDropIndicator(rect, false, white);
                else if (dropTrackIndex == sourceIndex + 1) DrawTrackDropIndicator(rect, true, white);
            }
        }
        target->PopAxisAlignedClip();
        if (rows.empty()) {
            Win32Ui::Impl::DrawText(L"< NO MATCHING TRACKS >", bounds, greenDim, regularFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }






void Win32Ui::Impl::DrawMini(const D2D1_SIZE_F size,
                   std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
        Win32Ui::Impl::DrawSkinDecor(size);
        screenBounds.clear();
        panelBounds.clear();
    moduleRegions.clear();
    decorControlBounds.clear();
    deferredTextLayouts.clear();
        deferTexts = true;
        const auto bounds = Rect(4, 4, size.width - 4, size.height - 4);
        auto content = DrawPanel(bounds, L"RIVAN // SHADE MODE", b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get());
        const auto lcd = Rect(content.left + 3, content.top + 3, content.right - 3, content.top + 49);
        // SCREEN: Mini-player LCD.
        Win32Ui::Impl::DrawBevel(lcd, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        Win32Ui::Impl::DrawText(FormatTime(model.positionSeconds), Rect(lcd.left + 5, lcd.top, lcd.left + 105, lcd.bottom),
                 b[6].Get(), digitalFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::DrawText(model.nowTitle, Rect(lcd.left + 111, lcd.top + 2, lcd.right - 5, lcd.top + 25),
                 b[6].Get(), regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::DrawText(model.nowArtist, Rect(lcd.left + 111, lcd.top + 24, lcd.right - 5, lcd.bottom - 2),
                 b[8].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        const float seekTop = lcd.bottom + 5;
        const float progress = model.durationSeconds > 0.0
            ? static_cast<float>(model.positionSeconds / model.durationSeconds) : 0.0F;
        Win32Ui::Impl::DrawSlider(Rect(content.left + 4, seekTop, content.right - 4, seekTop + 15), progress,
                   HitKind::Seek, b[5].Get(), b[13].Get(), b[2].Get(), b[3].Get(), b[4].Get());
        const float buttonTop = content.bottom - 29;
        Win32Ui::Impl::DrawButton(Rect(content.left + 4, buttonTop, content.left + 48, content.bottom - 3), L"|<<",
                   Command::Previous, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawButton(Rect(content.left + 52, buttonTop, content.left + 105, content.bottom - 3),
                   model.playback == PlaybackState::Playing ? L"||" : L">", Command::PlayPause,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get(),
                   model.playback == PlaybackState::Playing);
        Win32Ui::Impl::DrawButton(Rect(content.left + 109, buttonTop, content.left + 153, content.bottom - 3), L">>|",
                   Command::Next, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawText(L"VOL", Rect(content.right - 181, buttonTop, content.right - 151, content.bottom - 3),
                 b[13].Get(), tinyFormat.Get());
        Win32Ui::Impl::DrawSlider(Rect(content.right - 148, buttonTop + 5, content.right - 4, content.bottom - 8), model.volume,
                   HitKind::Volume, b[5].Get(), b[13].Get(), b[2].Get(), b[3].Get(), b[4].Get());
        Win32Ui::Impl::DrawSkinDecor(size, 1);
        Win32Ui::Impl::DrawSkinDecor(size, 2);
        Win32Ui::Impl::FlushDeferredTexts();
        Win32Ui::Impl::DrawImageSelection(size);
    }

// Keeps a click-to-play mono-selection in sync with the transport. A plain track click
    // both selects and plays the row, so the played track lands in trackSelection. When the
    // transport auto-advances, the new track's `playing` flag already highlights it; without
    // this the previous row would keep its selection fill and look like it is still active.
    // Only the lone auto-selection of the previously playing row is moved; genuine multi- or
    // ctrl-selections are left untouched.
    void Win32Ui::Impl::SyncSelectionToPlayback() {
        // Fast path: the previously playing row usually still plays, so skip the
        // full model scan (O(N) per paint) until the playhead actually moves.
        if (lastPlayingModelIndex < model.tracks.size() &&
            model.tracks[lastPlayingModelIndex].playing) {
            return;
        }
        std::size_t nowPlaying = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < model.tracks.size(); ++i) {
            if (model.tracks[i].playing) { nowPlaying = i; break; }
        }
        if (nowPlaying == lastPlayingModelIndex) return;
        // The playing row changed. If the selection is exactly the row that was playing,
        // it came from click-to-play, so hand it off to the new playing row (or drop it
        // when playback stopped) instead of stranding a stale highlight.
        if (lastPlayingModelIndex != static_cast<std::size_t>(-1) &&
            trackSelection.size() == 1 && trackSelection.contains(lastPlayingModelIndex)) {
            trackSelection.clear();
            if (nowPlaying != static_cast<std::size_t>(-1)) {
                trackSelection.insert(nowPlaying);
                trackAnchor = nowPlaying;
            } else {
                trackAnchor = static_cast<std::size_t>(-1);
            }
        }
        lastPlayingModelIndex = nowPlaying;
    }

} // namespace rivan::ui
