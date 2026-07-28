// Win32Ui.SkinStudio.cpp
// Skin Studio methods for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

// ---- Skin Studio ----------------------------------------------------------


[[nodiscard]] const std::array<Win32Ui::Impl::ColorField, 13>& Win32Ui::Impl::StudioColorFields() {
    static const std::array<ColorField, 13> fields{{
        {L"Window background", &skin::SkinPalette::windowBackground},
        {L"Panel background", &skin::SkinPalette::panelBackground},
        {L"Control background", &skin::SkinPalette::raisedBackground},
        {L"Text primary", &skin::SkinPalette::textPrimary},
        {L"Text secondary", &skin::SkinPalette::textSecondary},
        {L"Accent", &skin::SkinPalette::accent},
        {L"Hover background", &skin::SkinPalette::hoverBackground},
        {L"Border", &skin::SkinPalette::border},
        {L"Selection", &skin::SkinPalette::selection},
        {L"Screen color", &skin::SkinPalette::screenBackground},
        {L"Controls", &skin::SkinPalette::playbackProgress},
        {L"Visualization A", &skin::SkinPalette::visualizationPrimary},
        {L"Visualization B", &skin::SkinPalette::visualizationSecondary},
    }};
    return fields;
}



// Seeds the working draft from the active skin the first time the studio opens so
// edits start from what the user currently sees.
void Win32Ui::Impl::EnsureStudioDraft() {
    if (studioOpen) return;
    studioDraft = model.activeSkin;
    studioOpen = true;
    studioColorIndex = 0;
    studioSection = StudioSection::General;
    studioHex.clear();
    studioHexEditing = false;
    studioHexSelectAll = false;
    studioColorPickerVisible = false;
    studioColorTarget = StudioColorTarget::Palette;
    pickingScreenColor = false;
    eyedropperSkipUp = false;
    studioImageIndex = studioDraft.images.empty() ? 0 : studioDraft.images.size() - 1;
    studioImageScroll = studioImageIndex >= 2 ? studioImageIndex - 2 : 0;
}



void Win32Ui::Impl::PushPreview() {
    try { host.PreviewSkin(studioDraft); } catch (...) {}
    model.activeSkin = studioDraft;
    lastPreviewTick = GetTickCount64();
    previewPending = false;
    InvalidateRect(window, nullptr, FALSE);
}



void Win32Ui::Impl::QueuePreview() {
    model.activeSkin = studioDraft;
    previewPending = true;
    if (GetTickCount64() - lastPreviewTick >= kRefreshMilliseconds) PushPreview();
    else InvalidateRect(window, nullptr, FALSE);
}



void Win32Ui::Impl::StudioButton(const D2D1_RECT_F& bounds, const std::wstring& label, std::uint64_t action,
                  std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b, bool active,
                  IDWriteTextFormat* format) {
    const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
    DrawBevel(bounds, (hot || active) ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(), active);
    DrawText(label, bounds, b[9].Get(), format ? format : smallFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    HitRegion hit;
    hit.bounds = bounds;
    hit.kind = HitKind::Studio;
    hit.id = action;
    hits.push_back(hit);
}



// Same visual as StudioButton but routes clicks to HandleSettingsAction.
void Win32Ui::Impl::SettingsButton(const D2D1_RECT_F& bounds, const std::wstring& label, std::uint64_t action,
                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const bool integrationCategory = model.settingsCategory == SettingCategory::General ||
                                      model.settingsCategory == SettingCategory::Appearance ||
                                      model.settingsCategory == SettingCategory::Discord ||
                                     model.settingsCategory == SettingCategory::Downloading;
    const float clipTop = settingsDetailsBounds.top + (integrationCategory ? 15.0F : 50.0F);
    const float clipBottom = settingsDetailsBounds.bottom - 4.0F;
    const bool inViewport = windowKind != WindowKind::Settings ||
                            (bounds.bottom > clipTop && bounds.top < clipBottom);
    D2D1_RECT_F hitBounds = bounds;
    if (windowKind == WindowKind::Settings && inViewport) {
        hitBounds.top = std::max(bounds.top, clipTop);
        hitBounds.bottom = std::min(bounds.bottom, clipBottom);
    }
    const bool hot = inViewport &&
                     Contains(hitBounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
    DrawBevel(bounds, hot ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(), false);
    DrawText(label, bounds, b[9].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!inViewport || hitBounds.bottom <= hitBounds.top) return;
    HitRegion hit;
    hit.bounds = hitBounds;
    hit.kind = HitKind::SettingsAction;
    hit.id = action;
    hits.push_back(hit);
}



// General-pane actions: 100+N browse folder index N, 200+N clear folder index N,
// 4 toggle YouTube, 5 install yt-dlp, 6 install ffmpeg, 7/8 YT audio quality (shared),
// 9 cycle download format (MP3/Original/Video), 10/11 video height, 14 file preview,
// 15 start at startup, 16 exit to tray, 17 duplicate mode, 18 Discord rich presence,
// 19 clear the optional Discord image URL, 20 artist, 21 image tooltip text,
// 22 track covers, 23 Discord GitHub repo button.
void Win32Ui::Impl::HandleSettingsAction(std::uint64_t action) {
    try {
        if (action >= 100 && action < 200) {
            if (auto folder = PickFolder()) {
                host.SetMusicFolder(static_cast<std::size_t>(action - 100), *folder);
            }
            return;
        }
        if (action >= 200 && action < 300) {
            host.SetMusicFolder(static_cast<std::size_t>(action - 200),
                                std::filesystem::path{});
            return;
        }
        switch (action) {
        case 4:
            host.SetYoutubeEnabled(!model.youtubeEnabled);
            break;
        case 5:
            if (!model.youtubeYtDlpInstalled && !model.youtubeInstallingYtDlp) {
                host.InstallYoutubeTool(true);
            }
            break;
        case 6:
            if (!model.youtubeFfmpegInstalled && !model.youtubeInstallingFfmpeg) {
                host.InstallYoutubeTool(false);
            }
            break;
        case 7:
            // Audio quality is shared by every format, so always active when enabled.
            if (model.youtubeEnabled) {
                host.SetYoutubeAudioQuality(model.youtubeAudioQuality - 1);
            }
            break;
        case 8:
            if (model.youtubeEnabled) {
                host.SetYoutubeAudioQuality(model.youtubeAudioQuality + 1);
            }
            break;
        case 9:
            // Cycle download format: MP3 (0) -> Original (1) -> Video (2) -> MP3.
            host.SetYoutubeDownloadMode((model.youtubeDownloadMode + 1) % 3);
            break;
        case 10:
            if (model.youtubeDownloadMode == 2) {
                host.SetYoutubeMp4VideoQuality(model.youtubeMp4VideoQuality - 1);
            }
            break;
        case 11:
            if (model.youtubeDownloadMode == 2) {
                host.SetYoutubeMp4VideoQuality(model.youtubeMp4VideoQuality + 1);
            }
            break;
        case 14:
            host.SetFilePreviewEnabled(!model.filePreviewEnabled);
            break;
        case 15:
            host.SetStartAtStartup(!model.startAtStartup);
            break;
        case 16:
            host.SetExitToTray(!model.exitToTray);
            break;
        case 17:
            host.SetDuplicateAsFile(!model.duplicateAsFile);
            break;
        case 18:
            host.SetDiscordEnabled(!model.discordEnabled);
            break;
        case 19: {
            std::wstring error;
            if (!host.SetDiscordImageUrl(L"", error) && !error.empty()) {
                MessageBoxW(window, error.c_str(), L"Discord image URL",
                            MB_OK | MB_ICONWARNING);
            }
            discordImageEditing = false;
            discordImageSelectAll = false;
            discordImageBuffer.clear();
            break;
        }
        case 20:
            host.SetDiscordShowArtist(!model.discordShowArtist);
            break;
        case 21:
            host.SetDiscordShowImageText(!model.discordShowImageText);
            break;
        case 22:
            host.SetTrackCoverArtEnabled(!model.trackCoverArtEnabled);
            break;
        case 23:
            host.SetDiscordShowGithubButton(!model.discordShowGithubButton);
            break;
        case 50:
            // Youtube search GO button (main library pane).
            if (window) KillTimer(window, kYoutubeSearchDebounceTimer);
            host.SubmitYoutubeQuery(playlistQuery);
            break;
        case 51:
            if (model.youtubeCanPagePrev && model.youtubePage > 0) {
                host.SetYoutubeSearchPage(model.youtubePage - 1);
            }
            break;
        case 52:
            if (model.youtubeCanPageNext) {
                host.SetYoutubeSearchPage(model.youtubePage + 1);
            }
            break;
        case 53:
            // Youtube pane: plain YouTube search/download.
            host.SetYoutubeMusicSearch(false);
            break;
        case 54:
            // Youtube pane: YouTube Music catalog search/download.
            host.SetYoutubeMusicSearch(true);
            break;
        default: break;
        }
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}



// Opens the Windows folder picker (IFileDialog with FOS_PICKFOLDERS). Returns the
// chosen path, or nullopt if cancelled or unavailable.
[[nodiscard]] std::optional<std::filesystem::path> Win32Ui::Impl::PickFolder() {
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.GetAddressOf())))) {
        return std::nullopt;
    }
    DWORD dialogOptions = 0;
    if (SUCCEEDED(dialog->GetOptions(&dialogOptions))) {
        dialog->SetOptions(dialogOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    }
    if (FAILED(dialog->Show(window))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}



// Draws one left-rail section button with an icon glyph and highlights the active
// section. The label sits under the icon so the rail stays narrow.
void Win32Ui::Impl::StudioRailButton(const D2D1_RECT_F& bounds, const wchar_t* icon, const wchar_t* label,
                      StudioSection section, std::uint64_t action,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const bool active = studioSection == section;
    const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
    if (active) target->FillRectangle(bounds, b[11].Get());
    else if (hot) target->FillRectangle(bounds, b[7].Get());
    DrawText(icon, Rect(bounds.left, bounds.top + 3, bounds.right, bounds.bottom - 17),
             active ? b[12].Get() : b[6].Get(), studioIconFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(label, Rect(bounds.left, bounds.bottom - 16, bounds.right, bounds.bottom - 2),
             active ? b[12].Get() : b[10].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    HitRegion hit;
    hit.bounds = bounds;
    hit.kind = HitKind::Studio;
    hit.id = action;
    hits.push_back(hit);
}



// --- Studio section: Colors -------------------------------------------------
// Scrollable list of every semantic color with a live swatch; the selected color
// gets a HEX input (paste-friendly) plus RGBA steppers for fine control.
template <typename LabelFn, typename RowFn>
void Win32Ui::Impl::DrawStudioColors(float left, float right, float& y, LabelFn label, RowFn row,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const auto& fields = StudioColorFields();
    studioColorIndex = std::min(studioColorIndex, fields.size() - 1);
    label(L"COLORS  (click swatch to pick)");
    // Compact list: one clickable swatch+name per color.
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto r = Rect(left, y, right, y + 18);
        y += 19;
        const bool selected = i == studioColorIndex;
        const bool hot = Contains(r, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        if (selected) target->FillRectangle(r, b[11].Get());
        else if (hot) target->FillRectangle(r, b[7].Get());
        const skin::Color c = studioDraft.colors.*(fields[i].member);
        ComPtr<ID2D1SolidColorBrush> swatch;
        if (SUCCEEDED(target->CreateSolidColorBrush(ToD2D(c), swatch.ReleaseAndGetAddressOf()))) {
            DrawBevel(Rect(r.left + 2, r.top + 2, r.left + 22, r.bottom - 2), swatch.Get(),
                      b[3].Get(), b[4].Get(), true);
        }
        DrawText(fields[i].name, Rect(r.left + 28, r.top, r.right - 62, r.bottom),
                 selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
        const std::string hex = skin::FormatColor(c);
        DrawText(std::wstring(hex.begin(), hex.end()), Rect(r.right - 60, r.top, r.right - 2, r.bottom),
                 selected ? b[12].Get() : b[10].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        HitRegion hit;
        hit.bounds = r;
        hit.kind = HitKind::Studio;
        hit.id = kSelectColorBase + i;
        hits.push_back(hit);
        hit.bounds = Rect(r.left + 2, r.top + 2, r.left + 22, r.bottom - 2);
        hit.id = kOpenColorPickerBase + i;
        hits.push_back(hit);
    }
    y += 6;
    if (studioColorPickerVisible && studioColorTarget == StudioColorTarget::Palette) {
        DrawStudioColorPicker(left, right, y, row, b,
                              Action(StudioAction::ScreenEyedropper), 90);
    } else {
        studioColorPickerBounds = {};
        studioHueBounds = {};
        // Keep eyedropper + HEX available even when SV picker is collapsed.
        {
            const auto r = row();
            StudioButton(r, pickingScreenColor ? L"CLICK SCREEN..." : L"EYEDROPPER",
                         Action(StudioAction::ScreenEyedropper), b, pickingScreenColor);
        }
        label(L"HEX");
        {
            const skin::Color c = studioDraft.colors.*(fields[studioColorIndex].member);
            const auto r = row();
            DrawBevel(r, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
            std::wstring shown;
            if (studioHexEditing) {
                shown = studioHex;
                if ((GetTickCount64() / 500ULL) % 2ULL == 0ULL) shown += L"_";
            } else {
                const std::string hex = skin::FormatColor(c);
                shown = std::wstring(hex.begin(), hex.end());
            }
            if (studioHexEditing && studioHexSelectAll) {
                target->FillRectangle(Rect(r.left + 3, r.top + 2, r.right - 3, r.bottom - 2),
                                      b[11].Get());
            }
            DrawText(shown, Rect(r.left + 5, r.top, r.right - 4, r.bottom),
                      studioHexSelectAll ? b[12].Get() : b[6].Get(),
                      regularFormat.Get());
            HitRegion hit;
            hit.bounds = r;
            hit.kind = HitKind::Studio;
            hit.id = 90;
            hits.push_back(hit);
        }
    }
}



// --- Studio section: Font ---------------------------------------------------
template <typename LabelFn, typename RowFn>
void Win32Ui::Impl::DrawStudioFont(float left, float right, float& y, LabelFn label, RowFn row,
                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    (void)left; (void)right; (void)y;
    const std::wstring family(studioDraft.typography.fontFamily.begin(),
                              studioDraft.typography.fontFamily.end());
    label(L"FONT");
    {
        const auto r = row();
        StudioButton(r, L"IMPORT...", 40, b);
    }
    {
        const auto r = row();
        const float sizeLeft = r.right - 118.0F;
        StudioButton(Rect(r.left, r.top, sizeLeft - 3, r.bottom), family.c_str(), 41, b, studioFontDropdown);
        StudioButton(Rect(sizeLeft, r.top, sizeLeft + 22, r.bottom), L"-", 42, b);
        wchar_t sizeText[16]{};
        std::swprintf(sizeText, std::size(sizeText), L"%.0f pt", studioDraft.typography.baseSize);
        DrawText(sizeText, Rect(sizeLeft + 26, r.top, r.right - 26, r.bottom), b[9].Get(),
                  smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(r.right - 22, r.top, r.right, r.bottom), L"+", 43, b);
    }
    if (studioFontDropdown) {
        if (!studioDraft.typography.customFontFile.empty()) {
            const auto r = row();
            const auto customPath = studioDraft.directory / studioDraft.typography.customFontFile;
            auto customFormat = BuildTextFormat(family, studioDraft.typography.baseSize,
                                                DWRITE_FONT_WEIGHT_NORMAL, customPath);
            StudioButton(r, (L"CUSTOM  " + studioDraft.typography.customFontFile.filename().wstring()).c_str(),
                         150, b, true, customFormat.Get());
        }
        label(L"SYSTEM FONTS");
        for (std::size_t index = 0; index < kFontChoices.size(); ++index) {
            const auto r = row();
            auto previewFormat = BuildTextFormat(kFontChoices[index], studioDraft.typography.baseSize,
                                                 DWRITE_FONT_WEIGHT_NORMAL);
            StudioButton(r, kFontChoices[index], 140 + index, b,
                         studioDraft.typography.customFontFile.empty() &&
                             family == kFontChoices[index],
                         previewFormat.Get());
        }
    }
    label(L"FONT BORDER");
    {
        const auto r = row();
        StudioButton(Rect(r.left, r.top, r.left + 22, r.bottom), L"-", 45, b);
        wchar_t border[16]{};
        std::swprintf(border, std::size(border), L"%.0f px", studioDraft.typography.borderSize);
        DrawText(border, Rect(r.left + 26, r.top, r.right - 26, r.bottom), b[9].Get(),
                 smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(r.right - 22, r.top, r.right, r.bottom), L"+", 46, b);
    }
}



// --- Studio section: Toggles ------------------------------------------------
template <typename LabelFn, typename RowFn>
void Win32Ui::Impl::DrawStudioToggles(float left, float right, float& y, LabelFn label, RowFn row,
                       std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    (void)left; (void)right; (void)y;
    label(L"APPEARANCE");
    {
        const auto r = row();
        StudioButton(Rect(r.left, r.top, r.left + Width(r) / 3 - 3, r.bottom), L"TEXT BTNS", 10, b,
                     studioDraft.appearance.transparentButtons);
        StudioButton(Rect(r.left + Width(r) / 3 + 2, r.top, r.left + 2 * Width(r) / 3 - 2, r.bottom),
                     L"TITLE BARS", 11, b, studioDraft.appearance.showTitleBars);
        StudioButton(Rect(r.left + 2 * Width(r) / 3 + 3, r.top, r.right, r.bottom), L"BORDERS", 12, b,
                       studioDraft.appearance.showPanelBorders);
    }
    {
        const auto r = row();
        StudioButton(r,
                      L"CENTERED TITLES", 13, b, studioDraft.appearance.centeredTitles);
    }
    label(L"PANEL OPACITY");
    {
        const auto r = row();
        wchar_t po[24]{};
        std::swprintf(po, std::size(po), L"%.0f%%", studioDraft.appearance.panelOpacity * 100.0F);
        StudioButton(Rect(r.left, r.top, r.left + 22, r.bottom), L"-", 72, b);
        DrawText(po, Rect(r.left + 26, r.top, r.left + 90, r.bottom), b[9].Get(),
                 smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(r.left + 94, r.top, r.left + 116, r.bottom), L"+", 73, b);
    }
    label(L"SCREEN OPACITY");
    {
        const auto r = row();
        wchar_t opacity[24]{};
        std::swprintf(opacity, std::size(opacity), L"%.0f%%",
                      studioDraft.appearance.screenOpacity * 100.0F);
        StudioButton(Rect(r.left, r.top, r.left + 22, r.bottom), L"-", 76, b);
        DrawText(opacity, Rect(r.left + 26, r.top, r.left + 90, r.bottom), b[9].Get(),
                 smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(r.left + 94, r.top, r.left + 116, r.bottom), L"+", 77, b);
    }
}



// --- Studio section: Elements -----------------------------------------------
template <typename LabelFn, typename RowFn>
void Win32Ui::Impl::DrawStudioElements(float left, float right, float& y, LabelFn label, RowFn row,
                        std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    {
        const auto r = row();
        StudioButton(Rect(r.left, r.top, r.left + Width(r) * 0.5F - 3, r.bottom),
                     L"MAIN", Action(StudioAction::ShowElementEditor), b, !studioLayersTab);
        StudioButton(Rect(r.left + Width(r) * 0.5F + 3, r.top, r.right, r.bottom),
                     L"LAYERS", Action(StudioAction::ShowLayers), b, studioLayersTab);
    }
    if (studioLayersTab) {
        studioImageListBounds = {};
        studioImageRows = 0;
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        label(L"LAYERS  (DRAG TO REORDER, 1 = TOP)");
        studioLayerBounds = Rect(left, y, right, studioPanelBounds.bottom - 66.0F);
        studioLayerRows = static_cast<std::size_t>(std::max(1.0F, std::floor(Height(studioLayerBounds) / 26.0F)));
        const std::size_t maximum = order.size() > studioLayerRows ? order.size() - studioLayerRows : 0;
        studioLayerScroll = std::min(studioLayerScroll, maximum);
        const std::size_t last = std::min(order.size(), studioLayerScroll + studioLayerRows);
        for (std::size_t position = studioLayerScroll; position < last; ++position) {
            const auto& ref = order[position];
            const auto item = Rect(left, y, right, y + 22.0F);
            y += 26.0F;
            const bool selected = ref.image
                ? studioImageFocused && studioImageIndex == ref.index
                : studioShapeFocused && studioShapeIndex == ref.index;
            if (selected) target->FillRectangle(item, b[11].Get());
            DrawText(std::to_wstring(position + 1),
                     Rect(item.left + 3, item.top, item.left + 29, item.bottom),
                     selected ? b[12].Get() : b[6].Get(), smallFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            std::wstring name;
            if (ref.image) {
                if (auto* bitmap = LoadSkinBitmap(studioDraft.images[ref.index].file)) {
                    target->DrawBitmap(bitmap,
                        Rect(item.left + 31, item.top + 2, item.left + 51, item.bottom - 2),
                        1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                name = studioDraft.images[ref.index].file.filename().wstring();
            } else {
                const auto kind = studioDraft.shapes[ref.index].kind;
                name = kind == skin::ShapeKind::Ellipse ? L"SHAPE  OVAL"
                     : kind == skin::ShapeKind::Line ? L"SHAPE  LINE" : L"SHAPE  RECTANGLE";
            }
            DrawText(name, Rect(item.left + (ref.image ? 56.0F : 32.0F), item.top,
                                item.right - 5, item.bottom),
                      selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
            AddIdHit(item, HitKind::Studio, kSelectLayerBase + position);
        }
        if (order.empty()) {
            DrawText(L"Import image or add shape to create layer.", Rect(left, y, right, y + 28),
                     b[10].Get(), smallFormat.Get());
        }
        return;
    }

    const auto drawValueRow = [&](const wchar_t* name, float value, std::uint64_t decrease,
                                  std::uint64_t increase, bool percent) {
        const auto bounds = row();
        wchar_t text[48]{};
        if (percent) std::swprintf(text, std::size(text), L"%s  %.0f%%", name, value * 100.0F);
        else std::swprintf(text, std::size(text), L"%s  %.2f", name, value);
        StudioButton(Rect(bounds.left, bounds.top, bounds.left + 24, bounds.bottom),
                     L"-", decrease, b);
        DrawText(text, Rect(bounds.left + 28, bounds.top, bounds.right - 28, bounds.bottom),
                 b[9].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(bounds.right - 24, bounds.top, bounds.right, bounds.bottom),
                     L"+", increase, b);
    };
    auto drawImageControls = [&]() {
        auto& image = studioDraft.images[studioImageIndex];
        drawValueRow(L"X", image.x, 110, 111, false);
        drawValueRow(L"Y", image.y, 112, 113, false);
        drawValueRow(L"SIZE", image.width, 114, 115, true);
        drawValueRow(L"OPACITY", image.opacity, 116, 117, true);
        {
            const auto recolor = row();
            const float half = Width(recolor) * 0.5F;
            const bool tintActive = studioColorPickerVisible &&
                studioColorTarget == StudioColorTarget::ImageTint;
            StudioButton(Rect(recolor.left, recolor.top, recolor.left + half - 3, recolor.bottom),
                         L"TINT COLOR", Action(StudioAction::OpenImageTint), b, tintActive);
            StudioButton(Rect(recolor.left + half + 3, recolor.top, recolor.right, recolor.bottom),
                         L"CLEAR TINT", Action(StudioAction::ClearImageTint), b,
                         image.tint.alpha == 0);
            if (image.tint.alpha > 0) {
                ComPtr<ID2D1SolidColorBrush> swatch;
                if (SUCCEEDED(target->CreateSolidColorBrush(ToD2D(image.tint),
                        swatch.ReleaseAndGetAddressOf()))) {
                    DrawBevel(Rect(recolor.right - 48, recolor.top + 3, recolor.right - 28,
                                   recolor.bottom - 3), swatch.Get(), b[3].Get(), b[4].Get(), true);
                }
            }
        }
        if (studioColorPickerVisible && studioColorTarget == StudioColorTarget::ImageTint) {
            label(L"IMAGE TINT  (accent wash)");
            DrawStudioColorPicker(left, right, y, row, b,
                                  Action(StudioAction::ElementEyedropper),
                                  Action(StudioAction::ElementHexEdit));
        }
        const auto actions = row();
        const float cell = Width(actions) / 4.0F;
        StudioButton(Rect(actions.left, actions.top, actions.left + cell - 3, actions.bottom), L"DUPLICATE", 118, b);
        StudioButton(Rect(actions.left + cell, actions.top, actions.left + 2 * cell - 3, actions.bottom), L"FLIP H", 120, b, image.flipHorizontal);
        StudioButton(Rect(actions.left + 2 * cell, actions.top, actions.left + 3 * cell - 3, actions.bottom), L"FLIP V", 121, b, image.flipVertical);
        StudioButton(Rect(actions.left + 3 * cell, actions.top, actions.right, actions.bottom), L"DELETE", 119, b);
        const auto toggles = row();
        StudioButton(Rect(toggles.left, toggles.top, toggles.left + Width(toggles) * 0.5F - 3, toggles.bottom), L"IMG OVER PANELS", Action(StudioAction::ToggleImageOverPanels), b, image.overPanels);
        StudioButton(Rect(toggles.left + Width(toggles) * 0.5F + 3, toggles.top, toggles.right, toggles.bottom), L"IMG OVER SCREENS", Action(StudioAction::ToggleImageOverScreens), b, image.overScreens);
    };
    auto drawShapeControls = [&]() {
        auto& shape = studioDraft.shapes[studioShapeIndex];
        drawValueRow(L"X", shape.x, 130, 131, false);
        drawValueRow(L"Y", shape.y, 132, 133, false);
        drawValueRow(L"SIZE", shape.width, 134, 135, true);
        drawValueRow(L"OPACITY", shape.opacity, 136, 137, true);
        {
            const auto recolor = row();
            const bool recolorActive = studioColorPickerVisible &&
                studioColorTarget == StudioColorTarget::Shape;
            StudioButton(Rect(recolor.left, recolor.top, recolor.left + 96, recolor.bottom),
                         L"RECOLOR", Action(StudioAction::OpenShapeRecolor), b, recolorActive);
            ComPtr<ID2D1SolidColorBrush> swatch;
            skin::Color swatchColor = shape.color;
            swatchColor.alpha = 255;
            if (SUCCEEDED(target->CreateSolidColorBrush(ToD2D(swatchColor),
                    swatch.ReleaseAndGetAddressOf()))) {
                DrawBevel(Rect(recolor.left + 102, recolor.top + 3, recolor.left + 122,
                               recolor.bottom - 3), swatch.Get(), b[3].Get(), b[4].Get(), true);
            }
            const std::string hex = skin::FormatColor(swatchColor);
            DrawText(std::wstring(hex.begin(), hex.end()),
                     Rect(recolor.left + 128, recolor.top, recolor.right, recolor.bottom),
                     b[10].Get(), tinyFormat.Get());
        }
        if (studioColorPickerVisible && studioColorTarget == StudioColorTarget::Shape) {
            label(L"SHAPE COLOR");
            DrawStudioColorPicker(left, right, y, row, b,
                                  Action(StudioAction::ElementEyedropper),
                                  Action(StudioAction::ElementHexEdit));
        }
        if (!shape.filled) {
            const auto border = row();
            wchar_t text[48]{};
            std::swprintf(text, std::size(text), L"BORDER SIZE  %.0f px", shape.strokeWidth);
            StudioButton(Rect(border.left, border.top, border.left + 24, border.bottom), L"-", 127, b);
            DrawText(text, Rect(border.left + 28, border.top, border.right - 28, border.bottom), b[9].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            StudioButton(Rect(border.right - 24, border.top, border.right, border.bottom), L"+", 128, b);
        }
        const auto actions = row();
        const float actionCell = Width(actions) / 4.0F;
        StudioButton(Rect(actions.left, actions.top, actions.left + actionCell - 3, actions.bottom), L"DUPLICATE", 138, b);
        StudioButton(Rect(actions.left + actionCell, actions.top, actions.left + 2 * actionCell - 3, actions.bottom), L"FLIP H", 139, b, shape.flipHorizontal);
        StudioButton(Rect(actions.left + 2 * actionCell, actions.top, actions.left + 3 * actionCell - 3, actions.bottom), L"FLIP V", 146, b, shape.flipVertical);
        StudioButton(Rect(actions.left + 3 * actionCell, actions.top, actions.right, actions.bottom), L"DELETE", 147, b);
        const auto toggles = row();
        const float toggleCell = Width(toggles) / 3.0F;
        StudioButton(Rect(toggles.left, toggles.top, toggles.left + toggleCell - 3, toggles.bottom), L"OVER SCREENS", Action(StudioAction::ToggleShapeOverScreens), b, shape.overScreens);
        StudioButton(Rect(toggles.left + toggleCell, toggles.top, toggles.left + 2 * toggleCell - 3, toggles.bottom), L"TOGGLE FILL", Action(StudioAction::ToggleShapeFill), b, shape.filled);
        StudioButton(Rect(toggles.left + 2 * toggleCell, toggles.top, toggles.right, toggles.bottom), L"OVER PANELS", Action(StudioAction::ToggleShapeOverPanels), b, shape.overPanels);
    };

    wchar_t imageTitle[32]{};
    std::swprintf(imageTitle, std::size(imageTitle), L"IMAGES (%zu / 32)", studioDraft.images.size());
    label(imageTitle);
    {
        const auto r = row();
        StudioButton(r, L"IMPORT IMAGE", Action(StudioAction::ImportImage), b);
    }
    if (studioDraft.images.empty()) {
        studioImageListBounds = {};
        studioImageRows = 0;
        DrawText(L"No images.", Rect(left, y, right, y + 20), b[10].Get(), smallFormat.Get());
        y += 22.0F;
    } else {
        studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
        studioImageRows = std::min<std::size_t>(3, studioDraft.images.size());
        const std::size_t maximum = studioDraft.images.size() - studioImageRows;
        studioImageScroll = std::min(studioImageScroll, maximum);
        studioImageListBounds = Rect(left, y, right, y + studioImageRows * 24.0F);
        const std::size_t last = std::min(studioDraft.images.size(), studioImageScroll + studioImageRows);
        for (std::size_t index = studioImageScroll; index < last; ++index) {
            const auto item = Rect(left, y, right, y + 22.0F);
            y += 24.0F;
            const bool selected = studioImageFocused && index == studioImageIndex;
            if (selected) target->FillRectangle(item, b[11].Get());
            if (auto* bitmap = LoadSkinBitmap(studioDraft.images[index].file)) {
                target->DrawBitmap(bitmap, Rect(item.left + 2, item.top + 2,
                                   item.left + 22, item.bottom - 2), 1.0F,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                DrawText(L"?", Rect(item.left + 2, item.top, item.left + 22, item.bottom),
                         b[10].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            DrawText(std::to_wstring(index + 1) + L"  " + studioDraft.images[index].file.filename().wstring(),
                     Rect(item.left + 26, item.top, item.right - 5, item.bottom),
                     selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
            AddIdHit(item, HitKind::Studio, kSelectImageBase + index);
        }
        if (studioImageFocused) drawImageControls();
    }

    wchar_t shapeTitle[32]{};
    std::swprintf(shapeTitle, std::size(shapeTitle), L"SHAPES (%zu / 64)", studioDraft.shapes.size());
    label(shapeTitle);
    {
        const auto r = row();
        const float cell = Width(r) / 3.0F;
        StudioButton(Rect(r.left, r.top, r.left + cell - 3, r.bottom), L"+RECT", Action(StudioAction::AddRectangle), b);
        StudioButton(Rect(r.left + cell, r.top, r.left + 2 * cell - 3, r.bottom), L"+OVAL", Action(StudioAction::AddEllipse), b);
        StudioButton(Rect(r.left + 2 * cell, r.top, r.right, r.bottom), L"+LINE", Action(StudioAction::AddLine), b);
    }
    if (studioDraft.shapes.empty()) {
        DrawText(L"No shapes.", Rect(left, y, right, y + 20), b[10].Get(), smallFormat.Get());
        y += 22.0F;
    } else {
        studioShapeIndex = std::min(studioShapeIndex, studioDraft.shapes.size() - 1);
        const std::size_t first = studioShapeIndex >= 1 ? studioShapeIndex - 1 : 0;
        const std::size_t last = std::min(studioDraft.shapes.size(), first + 2);
        for (std::size_t index = first; index < last; ++index) {
            const auto item = Rect(left, y, right, y + 20.0F);
            y += 22.0F;
            const bool selected = studioShapeFocused && index == studioShapeIndex;
            if (selected) target->FillRectangle(item, b[11].Get());
            const auto kind = studioDraft.shapes[index].kind;
            const wchar_t* name = kind == skin::ShapeKind::Ellipse ? L"OVAL"
                : kind == skin::ShapeKind::Line ? L"LINE" : L"RECTANGLE";
            DrawText(std::to_wstring(index + 1) + L"  " + name,
                     Rect(item.left + 5, item.top, item.right - 5, item.bottom),
                     selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
            AddIdHit(item, HitKind::Studio, kSelectShapeBase + index);
        }
        if (studioShapeFocused) drawShapeControls();
    }
}



void Win32Ui::Impl::DrawSkinStudio(const D2D1_SIZE_F size,
                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    EnsureStudioDraft();
    target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
    const auto panel = Rect(10.0F, 10.0F, size.width - 10.0F, size.height - 10.0F);
    studioPanelBounds = panel;
    auto content = DrawPanel(panel, L"SKIN STUDIO", b[1].Get(), b[2].Get(), b[3].Get(),
                             b[4].Get(), b[13].Get(), b[7].Get());
    captionRect = Rect(panel.left + 4, panel.top + 4, content.right - 67, panel.top + 22);
    StudioButton(Rect(content.right - 62, content.top + 3, content.right - 3, content.top + 24),
                 L"CLOSE", 2, b);

    // Left icon rail: one button per customization section.
    const float railWidth = std::clamp(Width(content) * 0.16F, 62.0F, 105.0F);
    const auto rail = Rect(content.left + 2, content.top + 30, content.left + 2 + railWidth,
                           content.bottom - 30);
    // SCREEN: Skin Studio section rail.
    DrawBevel(rail, b[5].Get(), b[3].Get(), b[4].Get(), true);
    struct RailItem { const wchar_t* icon; const wchar_t* label; StudioSection section; std::uint64_t action; };
    const std::array<RailItem, 3> items{{
        {L"\u2699", L"GENERAL", StudioSection::General, Action(StudioAction::SelectGeneral)},    // gear
        {L"\U0001FAA3", L"COLORS", StudioSection::Colors, Action(StudioAction::SelectColors)},  // bucket
        {L"\U0001F5BC", L"ELEMENTS", StudioSection::Elements, Action(StudioAction::SelectElements)},  // picture
    }};
    float railTop = rail.top + 4;
    const float itemHeight = std::clamp(Height(rail) / 3.0F, 52.0F, 86.0F);
    for (const auto& item : items) {
        StudioRailButton(Rect(rail.left + 3, railTop, rail.right - 3, railTop + itemHeight - 4),
                         item.icon, item.label, item.section, item.action, b);
        railTop += itemHeight;
    }

    // Right detail pane for the active section.
    const auto pane = Rect(rail.right + 6, content.top + 30, content.right - 3, content.bottom - 30);
    // SCREEN: Skin Studio detail pane.
    DrawBevel(pane, b[5].Get(), b[3].Get(), b[4].Get(), true);
    float y = pane.top + 8;
    const float left = pane.left + 8;
    const float right = pane.right - 8;
    auto label = [&](const std::wstring& text) {
        DrawText(text, Rect(left, y, right, y + 16), b[6].Get(), smallFormat.Get());
        y += 18;
    };
    auto row = [&]() {
        const auto r = Rect(left, y, right, y + 22);
        y += 26;
        return r;
    };
    switch (studioSection) {
    case StudioSection::Colors: DrawStudioColors(left, right, y, label, row, b); break;
    case StudioSection::General:
        DrawStudioToggles(left, right, y, label, row, b);
        DrawStudioFont(left, right, y, label, row, b);
        break;
    case StudioSection::Elements: DrawStudioElements(left, right, y, label, row, b); break;
    }

    // --- Save / Cancel ---
    const auto footer = Rect(content.left + 8, content.bottom - 26, content.right - 8, content.bottom - 4);
    if (studioNameEditing) {
        const auto nameBox = Rect(footer.left, footer.top, footer.right - 82, footer.bottom);
        // SCREEN: Skin name text field.
        DrawBevel(nameBox, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        DrawText(studioName + (((GetTickCount64() / 500ULL) % 2ULL == 0ULL) ? L"_" : L""),
                 Rect(nameBox.left + 5, nameBox.top, nameBox.right - 4, nameBox.bottom), b[9].Get(),
                 regularFormat.Get());
        StudioButton(Rect(footer.right - 78, footer.top, footer.right, footer.bottom), L"SAVE", 96, b);
    } else {
        StudioButton(Rect(footer.left, footer.top, footer.left + Width(footer) * 0.5F - 3, footer.bottom),
                     L"SAVE SKIN", 1, b);
        StudioButton(Rect(footer.left + Width(footer) * 0.5F + 3, footer.top, footer.right, footer.bottom),
                     L"CANCEL", 3, b);
    }
}



// Opens a system file picker; returns the chosen path or empty on cancel/error.
[[nodiscard]] std::filesystem::path Win32Ui::Impl::PickFile(const COMDLG_FILTERSPEC* filters, UINT filterCount) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.ReleaseAndGetAddressOf())))) {
        return {};
    }
    dialog->SetFileTypes(filterCount, filters);
    if (FAILED(dialog->Show(window))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.ReleaseAndGetAddressOf()))) return {};
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}



void Win32Ui::Impl::StepColorChannel(int channelPair) {
    auto& color = studioDraft.colors.*(StudioColorFields()[studioColorIndex].member);
    const bool increment = (channelPair % 2) != 0;
    const int channel = channelPair / 2;  // 0=R 1=G 2=B 3=A
    auto step = [increment](std::uint8_t& value) {
        const int delta = increment ? 16 : -16;
        value = static_cast<std::uint8_t>(std::clamp(static_cast<int>(value) + delta, 0, 255));
    };
    switch (channel) {
    case 0: step(color.red); break;
    case 1: step(color.green); break;
    case 2: step(color.blue); break;
    case 3: step(color.alpha); break;
    default: break;
    }
}



[[nodiscard]] std::wstring Win32Ui::Impl::ToHexW(skin::Color color) {
    const std::string value = skin::FormatColor(color);
    return std::wstring(value.begin(), value.end());
}



// Sample the desktop pixel under the cursor (any monitor) into the active studio color.
// Keeps the previous alpha channel so only RGB is replaced (except image tint, which
// forces a visible accent alpha when previously cleared).
void Win32Ui::Impl::SampleScreenColorAtCursor() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const HDC screen = GetDC(nullptr);
    if (!screen) return;
    const COLORREF pixel = GetPixel(screen, cursor.x, cursor.y);
    ReleaseDC(nullptr, screen);
    if (pixel == CLR_INVALID) return;
    skin::Color* color = ActiveStudioColor();
    if (!color) return;
    color->red = GetRValue(pixel);
    color->green = GetGValue(pixel);
    color->blue = GetBValue(pixel);
    if (studioColorTarget == StudioColorTarget::Shape) {
        color->alpha = 255;
    } else if (studioColorTarget == StudioColorTarget::ImageTint && color->alpha == 0) {
        color->alpha = 160;
    }
    studioHex = ToHexW(*color);
    studioHexEditing = false;
    studioHexSelectAll = false;
    QueuePreview();
}



void Win32Ui::Impl::BeginScreenEyedropper() {
    if (studioColorTarget == StudioColorTarget::Palette &&
        studioSection != StudioSection::Colors) {
        SelectStudioSection(StudioSection::Colors);
    } else if (studioColorTarget != StudioColorTarget::Palette &&
               studioSection != StudioSection::Elements) {
        SelectStudioSection(StudioSection::Elements);
    }
    studioColorPickerVisible = true;
    pickingScreenColor = true;
    eyedropperSkipUp = true;
    draggingStudioColor = false;
    draggingStudioHue = false;
    studioHexEditing = false;
    studioHexSelectAll = false;
    SetCapture(window);
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    InvalidateRect(window, nullptr, FALSE);
}



void Win32Ui::Impl::CancelScreenEyedropper() {
    if (!pickingScreenColor) return;
    pickingScreenColor = false;
    eyedropperSkipUp = false;
    if (GetCapture() == window) ReleaseCapture();
    InvalidateRect(window, nullptr, FALSE);
}



void Win32Ui::Impl::ApplyStudioHex() {
    std::string value;
    value.reserve(studioHex.size());
    for (const wchar_t character : studioHex) value.push_back(static_cast<char>(character));
    const auto parsed = skin::ParseColor(value);
    if (!parsed) {
        MessageBoxW(window, L"Enter #RRGGBB or #RRGGBBAA.", L"Invalid HEX color",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    skin::Color* color = ActiveStudioColor();
    if (!color) return;
    *color = *parsed;
    if (studioColorTarget == StudioColorTarget::Shape) {
        color->alpha = 255;
    } else if (studioColorTarget == StudioColorTarget::ImageTint && color->alpha == 0) {
        color->alpha = 160;
    }
    studioHex = ToHexW(*color);
    studioHexEditing = false;
    studioHexSelectAll = false;
    PushPreview();
}



void Win32Ui::Impl::CopyStudioHex() {
    if (!OpenClipboard(window)) return;
    const std::size_t bytes = (studioHex.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (data != nullptr) {
        if (void* destination = GlobalLock(data)) {
            std::memcpy(destination, studioHex.c_str(), bytes);
            GlobalUnlock(data);
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, data) != nullptr) data = nullptr;
        }
        if (data != nullptr) GlobalFree(data);
    }
    CloseClipboard();
}



void Win32Ui::Impl::PasteStudioHex() {
    if (!OpenClipboard(window)) return;
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text) {
            studioHex.assign(text);
            GlobalUnlock(data);
            while (!studioHex.empty() && std::iswspace(studioHex.back())) studioHex.pop_back();
            std::size_t first = 0;
            while (first < studioHex.size() && std::iswspace(studioHex[first])) ++first;
            studioHex.erase(0, first);
            if (studioHex.size() > 9) studioHex.resize(9);
            studioHexEditing = true;
            studioHexSelectAll = false;
        }
    }
    CloseClipboard();
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::PastePlaylistQuery() {
    if (!OpenClipboard(window)) return;
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text) {
            if (playlistQuerySelectAll) playlistQuery.clear();
            for (const wchar_t* cursor = text; *cursor != L'\0' && playlistQuery.size() < 120; ++cursor) {
                const wchar_t ch = *cursor;
                if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
                    if (!playlistQuery.empty() && playlistQuery.back() != L' ') {
                        playlistQuery.push_back(L' ');
                    }
                    continue;
                }
                if (ch >= L' ' && ch != 0x7FU) playlistQuery.push_back(ch);
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    playlistQuerySelectAll = false;
    playlistSearchScroll = 0;
    ArmYoutubeSearchDebounce();
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::PasteDiscordImageUrl() {
    if (!OpenClipboard(window)) return;
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text) {
            if (discordImageSelectAll) discordImageBuffer.clear();
            for (const wchar_t* cursor = text;
                 *cursor != L'\0' && discordImageBuffer.size() < 2048; ++cursor) {
                const wchar_t ch = *cursor;
                if (ch >= L' ' && ch != 0x7FU) discordImageBuffer.push_back(ch);
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    discordImageSelectAll = false;
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CopyTrackName() {
    if (!OpenClipboard(window)) return;
    const std::size_t bytes = (trackNameBuffer.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (data != nullptr) {
        if (void* destination = GlobalLock(data)) {
            std::memcpy(destination, trackNameBuffer.c_str(), bytes);
            GlobalUnlock(data);
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, data) != nullptr) data = nullptr;
        }
        if (data != nullptr) GlobalFree(data);
    }
    CloseClipboard();
}

void Win32Ui::Impl::PasteTrackName() {
    if (!OpenClipboard(window)) return;
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data != nullptr) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text != nullptr) {
            trackNameCursor = std::min(trackNameCursor, trackNameBuffer.size());
            if (trackNameSelectAll) {
                trackNameBuffer.clear();
                trackNameCursor = 0;
            }
            for (const wchar_t* cursor = text; *cursor != L'\0' && trackNameBuffer.size() < 180; ++cursor) {
                if (*cursor >= L' ' && *cursor != 0x7FU) {
                    trackNameBuffer.insert(trackNameCursor, 1, *cursor);
                    ++trackNameCursor;
                }
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    trackNameSelectAll = false;
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CommitDiscordImageUrl() {
    if (!discordImageEditing) return;
    std::wstring error;
    if (!host.SetDiscordImageUrl(discordImageBuffer, error)) {
        if (!error.empty()) {
            MessageBoxW(window, error.c_str(), L"Discord image URL",
                        MB_OK | MB_ICONWARNING);
        }
        return;
    }
    discordImageEditing = false;
    discordImageSelectAll = false;
    discordImageBuffer.clear();
    InvalidateRect(window, nullptr, FALSE);
}



void Win32Ui::Impl::ImportStudioImage() {
    static const COMDLG_FILTERSPEC filters[] = {
        {L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.webp"}, {L"All files", L"*.*"}};
    const auto source = PickFile(filters, 2);
    if (source.empty()) return;
    std::wstring error;
    const auto relative = host.ImportSkinAsset(studioDraft.id, source,
                                               SkinAssetKind::BackgroundImage, error);
    if (!relative) {
        if (!error.empty()) MessageBoxW(window, error.c_str(), L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
        return;
    }
    if (studioDraft.images.size() < 32) {
        skin::SkinImage image;
        image.file = *relative;
        studioDraft.images.push_back(std::move(image));
        studioImageIndex = studioDraft.images.size() - 1;
        studioImageScroll = studioImageIndex >= 2 ? studioImageIndex - 2 : 0;
        studioImageFocused = true;
        studioShapeFocused = false;
        try { host.FocusSkinElement(-(static_cast<int>(studioImageIndex) + 1)); } catch (...) {}
    }
}



void Win32Ui::Impl::ImportStudioFont() {
    static const COMDLG_FILTERSPEC filters[] = {
        {L"Fonts", L"*.ttf;*.otf;*.ttc"}, {L"All files", L"*.*"}};
    const auto source = PickFile(filters, 2);
    if (source.empty()) return;
    std::wstring error;
    const auto relative = host.ImportSkinAsset(studioDraft.id, source, SkinAssetKind::Font, error);
    if (!relative) {
        if (!error.empty()) MessageBoxW(window, error.c_str(), L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
        return;
    }
    const auto importedFamily = FontFamilyFromFile(source);
    if (!importedFamily) {
        MessageBoxW(window, L"Unable to read imported font family.", L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
        return;
    }
    studioDraft.typography.customFontFile = *relative;
    studioDraft.typography.fontFamily = Utf8(*importedFamily);
}



void Win32Ui::Impl::HandleStudioAction(std::uint64_t action) {
    // Skin Manager (settings pane) actions operate on installed skins, not the studio
    // draft, so they are handled here without touching the preview.
    if (action == 100) {  // Open the skin studio editor
        try { host.Invoke(Command::ToggleSkinStudio); } catch (...) {}
        return;
    }
    if (action == 101) {  // Open the skins folder
        try { host.OpenSkinFolder(); } catch (...) {}
        return;
    }
    if (action >= 1400 && action < 1500) {
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        const std::size_t position = static_cast<std::size_t>(action - 1400);
        if (position < order.size()) {
            const auto ref = order[position];
            auto& priority = ref.image ? studioDraft.images[ref.index].priority
                                       : studioDraft.shapes[ref.index].priority;
            priority = static_cast<std::uint8_t>(std::min(99, static_cast<int>(priority) + 1));
            PushPreview();
        }
        return;
    }
    if (action >= 1300 && action < 1400) {
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        const std::size_t position = static_cast<std::size_t>(action - 1300);
        if (position < order.size()) {
            const auto ref = order[position];
            auto& priority = ref.image ? studioDraft.images[ref.index].priority
                                       : studioDraft.shapes[ref.index].priority;
            priority = static_cast<std::uint8_t>(std::max(1, static_cast<int>(priority) - 1));
            PushPreview();
        }
        return;
    }
    if (action >= kSelectLayerBase && action < kSelectLayerBase + 100) {
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        const std::size_t position = static_cast<std::size_t>(action - kSelectLayerBase);
        if (position < order.size()) {
            const auto ref = order[position];
            studioImageFocused = ref.image;
            studioShapeFocused = !ref.image;
            if (ref.image) studioImageIndex = ref.index;
            else studioShapeIndex = ref.index;
            try { host.FocusSkinElement(ref.image ? -(static_cast<int>(ref.index) + 1)
                                                  : static_cast<int>(ref.index) + 1); } catch (...) {}
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= 900 && action < 1000) {
        const std::size_t index = static_cast<std::size_t>(action - 900);
        if (managerNameEditing && index == managerSkinIndex && index < model.skins.size()) {
            std::wstring error;
            if (managerSkinName.empty() || !host.RenameSkin(model.skins[index].id,
                                                             managerSkinName, error)) {
                MessageBoxW(window, error.empty() ? L"Enter a skin name." : error.c_str(),
                            L"Rivan Skin Manager", MB_OK | MB_ICONWARNING);
                return;
            }
            managerNameEditing = false;
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= 800 && action < 900) {
        const std::size_t index = static_cast<std::size_t>(action - 800);
        if (index < model.skins.size() && !model.skins[index].builtIn &&
            MessageBoxW(window, (L"Delete saved skin '" + model.skins[index].name + L"'?").c_str(),
                        L"Rivan Skin Manager", MB_YESNO | MB_ICONWARNING) == IDYES) {
            std::wstring error;
            if (!host.DeleteSkin(model.skins[index].id, error)) {
                MessageBoxW(window, error.empty() ? L"Unable to delete skin." : error.c_str(),
                            L"Rivan Skin Manager", MB_OK | MB_ICONWARNING);
            }
        }
        return;
    }
    if (action >= 700 && action < 800) {
        const std::size_t index = static_cast<std::size_t>(action - 700);
        if (index < model.skins.size() && !model.skins[index].builtIn) {
            host.EditSkin(model.skins[index].id);
        }
        return;
    }
    if (action >= 600 && action < 700) {
        const std::size_t index = static_cast<std::size_t>(action - 600);
        if (index < model.skins.size() && !model.skins[index].builtIn) {
            managerSkinIndex = index;
            managerSkinName = model.skins[index].name;
            managerNameEditing = true;
            SetFocus(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
    }
    if (action >= kSelectImageBase && action < kSelectImageBase + 100) {
        const std::size_t index = static_cast<std::size_t>(action - kSelectImageBase);
        if (index < studioDraft.images.size()) {
            studioImageIndex = index;
            studioImageFocused = true;
            studioShapeFocused = false;
            if (studioColorTarget == StudioColorTarget::Shape) {
                studioColorPickerVisible = false;
                studioColorTarget = StudioColorTarget::Palette;
            }
            try { host.FocusSkinElement(-(static_cast<int>(index) + 1)); } catch (...) {}
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= kSelectShapeBase && action < kSelectShapeBase + 100) {
        const std::size_t index = static_cast<std::size_t>(action - kSelectShapeBase);
        if (index < studioDraft.shapes.size()) {
            studioShapeIndex = index;
            studioShapeFocused = true;
            studioImageFocused = false;
            if (studioColorTarget == StudioColorTarget::ImageTint) {
                studioColorPickerVisible = false;
                studioColorTarget = StudioColorTarget::Palette;
            }
            try { host.FocusSkinElement(static_cast<int>(index) + 1); } catch (...) {}
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    const auto colorCount = StudioColorFields().size();
    if (action >= kOpenColorPickerBase && action < kOpenColorPickerBase + colorCount) {
        studioColorIndex = static_cast<std::size_t>(action - kOpenColorPickerBase);
        studioColorTarget = StudioColorTarget::Palette;
        studioColorPickerVisible = true;
        studioHexEditing = false;
        studioHexSelectAll = false;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= kSelectColorBase && action < kSelectColorBase + colorCount) {
        studioColorIndex = static_cast<std::size_t>(action - kSelectColorBase);
        studioColorTarget = StudioColorTarget::Palette;
        studioHexEditing = false;
        studioHexSelectAll = false;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= 200) {  // Apply the skin at this list index
        const std::size_t index = static_cast<std::size_t>(action - 200);
        if (index < model.skins.size()) {
            try { host.ApplySkin(model.skins[index].id); } catch (...) {}
        }
        return;
    }
    // Left-rail section selection.
    switch (action) {
    case Action(StudioAction::SelectColors): SelectStudioSection(StudioSection::Colors); return;
    case Action(StudioAction::SelectGeneral): SelectStudioSection(StudioSection::General); return;
    case Action(StudioAction::SelectElements): SelectStudioSection(StudioSection::Elements); return;
    case Action(StudioAction::ShowElementEditor): studioLayersTab = false; InvalidateRect(window, nullptr, FALSE); return;
    case Action(StudioAction::ShowLayers): studioLayersTab = true; InvalidateRect(window, nullptr, FALSE); return;
    default: break;
    }
    // HEX / eyedropper / element recolor actions.
    if (action == Action(StudioAction::ScreenEyedropper) ||
        action == Action(StudioAction::ElementEyedropper)) {
        if (pickingScreenColor) CancelScreenEyedropper();
        else BeginScreenEyedropper();
        return;
    }
    if (action == Action(StudioAction::OpenShapeRecolor)) {
        OpenElementColorPicker(StudioColorTarget::Shape);
        return;
    }
    if (action == Action(StudioAction::OpenImageTint)) {
        OpenElementColorPicker(StudioColorTarget::ImageTint);
        return;
    }
    if (action == Action(StudioAction::ClearImageTint)) {
        if (!studioDraft.images.empty()) {
            studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
            studioDraft.images[studioImageIndex].tint = {0, 0, 0, 0};
            if (studioColorTarget == StudioColorTarget::ImageTint) {
                studioColorPickerVisible = false;
            }
            PushPreview();
        }
        return;
    }
    if (action == 90 || action == Action(StudioAction::ElementHexEdit)) {
        if (skin::Color* color = ActiveStudioColor()) {
            studioHex = ToHexW(*color);
            studioHexEditing = true;
            studioHexSelectAll = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
    }

    auto& appearance = studioDraft.appearance;
    auto& type = studioDraft.typography;
    const auto& fields = StudioColorFields();
    switch (action) {
    case 1:
        studioName.assign(studioDraft.name.begin(), studioDraft.name.end());
        studioNameEditing = true;
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case 96: {  // Save after naming
        if (studioName.empty()) {
            MessageBoxW(window, L"Enter a skin name before saving.", L"Rivan Skin Studio",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        studioDraft.name = Utf8(studioName);
        // Settings → Skin Studio forks a new skin; Edit on a saved skin overwrites.
        auto toSave = studioDraft;
        if (!model.skinStudioEditExisting) {
            toSave.id.clear();
            toSave.builtIn = true;
        }
        std::wstring error;
        if (!host.SaveSkin(toSave, error)) {
            MessageBoxW(window, error.empty() ? L"Unable to save skin." : error.c_str(),
                        L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
            return;
        }
        studioOpen = false;
        studioNameEditing = false;
        try { host.Invoke(Command::ToggleSkinStudio); } catch (...) {}
        return;
    }
    case 2:  // Close (keep preview)
    case 3:  // Cancel (revert preview)
        if (action == 3) { try { host.CancelSkinPreview(); } catch (...) {} }
        studioOpen = false;
        try { host.Invoke(Command::ToggleSkinStudio); } catch (...) {}
        return;
    case 10: appearance.transparentButtons = !appearance.transparentButtons; break;
    case 11: appearance.showTitleBars = !appearance.showTitleBars; break;
    case 12: appearance.showPanelBorders = !appearance.showPanelBorders; break;
    case 13: appearance.centeredTitles = !appearance.centeredTitles; break;
    case 20: if (studioDraft.shapes.size() < 64) { skin::SkinShape s; s.color = {255, 255, 255, 255}; studioDraft.shapes.push_back(s); studioShapeIndex = studioDraft.shapes.size() - 1; studioShapeFocused = true; studioImageFocused = false; try { host.FocusSkinElement(static_cast<int>(studioShapeIndex) + 1); } catch (...) {} } break;
    case 21: if (studioDraft.shapes.size() < 64) { skin::SkinShape s; s.kind = skin::ShapeKind::Ellipse; s.color = {255, 255, 255, 255}; studioDraft.shapes.push_back(s); studioShapeIndex = studioDraft.shapes.size() - 1; studioShapeFocused = true; studioImageFocused = false; try { host.FocusSkinElement(static_cast<int>(studioShapeIndex) + 1); } catch (...) {} } break;
    case 22: if (studioDraft.shapes.size() < 64) { skin::SkinShape s; s.kind = skin::ShapeKind::Line; s.filled = false; s.color = {255, 255, 255, 255}; studioDraft.shapes.push_back(s); studioShapeIndex = studioDraft.shapes.size() - 1; studioShapeFocused = true; studioImageFocused = false; try { host.FocusSkinElement(static_cast<int>(studioShapeIndex) + 1); } catch (...) {} } break;
    case 23: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].filled = !studioDraft.shapes[studioShapeIndex].filled; break;
    case 30: ImportStudioImage(); break;
    case 40: ImportStudioFont(); break;
    case 41: studioFontDropdown = !studioFontDropdown; InvalidateRect(window, nullptr, FALSE); return;
    case 45: type.borderSize = std::max(0.0F, type.borderSize - 1.0F); break;
    case 46: type.borderSize = std::min(8.0F, type.borderSize + 1.0F); break;
    case 140: case 141: case 142: case 143: case 144: case 145: { // Choose system font
        const std::size_t choice = static_cast<std::size_t>(action - 140);
        if (choice >= kFontChoices.size()) break;
        const std::wstring chosen = kFontChoices[choice];
        type.fontFamily = Utf8(chosen);
        type.customFontFile.clear();
        studioFontDropdown = false;
        break;
    }
    case 150: studioFontDropdown = false; break;
    case 42: type.baseSize = std::clamp(type.baseSize - 1.0F, 8.0F, 32.0F); break;
    case 43: type.baseSize = std::clamp(type.baseSize + 1.0F, 8.0F, 32.0F); break;
    case 44: type.customFontFile.clear(); break;
    case 50: studioColorIndex = (studioColorIndex + fields.size() - 1) % fields.size(); break;
    case 51: studioColorIndex = (studioColorIndex + 1) % fields.size(); break;
    case 72: appearance.panelOpacity = std::clamp(appearance.panelOpacity - 0.1F, 0.0F, 1.0F); break;
    case 73: appearance.panelOpacity = std::clamp(appearance.panelOpacity + 0.1F, 0.0F, 1.0F); break;
    case 76: appearance.screenOpacity = std::clamp(appearance.screenOpacity - 0.1F, 0.0F, 1.0F); break;
    case 77: appearance.screenOpacity = std::clamp(appearance.screenOpacity + 0.1F, 0.0F, 1.0F); break;
    case 110: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].x = std::max(-1.0F, studioDraft.images[studioImageIndex].x - 0.02F); break;
    case 111: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].x = std::min(2.0F, studioDraft.images[studioImageIndex].x + 0.02F); break;
    case 112: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].y = std::max(-1.0F, studioDraft.images[studioImageIndex].y - 0.02F); break;
    case 113: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].y = std::min(2.0F, studioDraft.images[studioImageIndex].y + 0.02F); break;
    case 114: if (!studioDraft.images.empty()) { auto& image = studioDraft.images[studioImageIndex]; const float ratio = image.height / image.width; image.width = std::max(0.02F, image.width - 0.02F); image.height = image.width * ratio; } break;
    case 115: if (!studioDraft.images.empty()) { auto& image = studioDraft.images[studioImageIndex]; const float ratio = image.height / image.width; image.width = std::min(2.0F, image.width + 0.02F); image.height = std::min(2.0F, image.width * ratio); } break;
    case 116: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].opacity = std::max(0.0F, studioDraft.images[studioImageIndex].opacity - 0.05F); break;
    case 117: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].opacity = std::min(1.0F, studioDraft.images[studioImageIndex].opacity + 0.05F); break;
    case 118:
        if (!studioDraft.images.empty() && studioDraft.images.size() < 32) {
            auto copy = studioDraft.images[studioImageIndex];
            copy.x = std::min(2.0F, copy.x + 0.03F);
            copy.y = std::min(2.0F, copy.y + 0.03F);
            studioDraft.images.insert(studioDraft.images.begin() + studioImageIndex + 1, std::move(copy));
            ++studioImageIndex;
            // Scroll the new duplicate into the 3-row window (mirrors ImportImage).
            studioImageScroll = studioImageIndex >= 2 ? studioImageIndex - 2 : 0;
        }
        break;
    case 119:
        if (!studioDraft.images.empty()) {
            studioDraft.images.erase(studioDraft.images.begin() + studioImageIndex);
            if (!studioDraft.images.empty()) studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
            else studioImageIndex = 0;
            studioImageFocused = !studioDraft.images.empty();
        }
        break;
    case 120: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].flipHorizontal = !studioDraft.images[studioImageIndex].flipHorizontal; break;
    case 121: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].flipVertical = !studioDraft.images[studioImageIndex].flipVertical; break;
    case 122: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].overPanels = !studioDraft.images[studioImageIndex].overPanels; break;
    case 123: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].overScreens = !studioDraft.images[studioImageIndex].overScreens; break;
    case 127: if (!studioDraft.shapes.empty() && !studioDraft.shapes[studioShapeIndex].filled) studioDraft.shapes[studioShapeIndex].strokeWidth = std::max(0.0F, studioDraft.shapes[studioShapeIndex].strokeWidth - 1.0F); break;
    case 128: if (!studioDraft.shapes.empty() && !studioDraft.shapes[studioShapeIndex].filled) studioDraft.shapes[studioShapeIndex].strokeWidth = std::min(32.0F, studioDraft.shapes[studioShapeIndex].strokeWidth + 1.0F); break;
    case 130: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].x = std::max(-1.0F, studioDraft.shapes[studioShapeIndex].x - 0.02F); break;
    case 131: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].x = std::min(2.0F, studioDraft.shapes[studioShapeIndex].x + 0.02F); break;
    case 132: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].y = std::max(-1.0F, studioDraft.shapes[studioShapeIndex].y - 0.02F); break;
    case 133: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].y = std::min(2.0F, studioDraft.shapes[studioShapeIndex].y + 0.02F); break;
    case 134: if (!studioDraft.shapes.empty()) { auto& s = studioDraft.shapes[studioShapeIndex]; s.width = std::max(0.02F, s.width - 0.02F); s.height = std::max(0.02F, s.height - 0.02F); } break;
    case 135: if (!studioDraft.shapes.empty()) { auto& s = studioDraft.shapes[studioShapeIndex]; s.width = std::min(2.0F, s.width + 0.02F); s.height = std::min(2.0F, s.height + 0.02F); } break;
    case 136: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].opacity = std::max(0.0F, studioDraft.shapes[studioShapeIndex].opacity - 0.05F); break;
    case 137: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].opacity = std::min(1.0F, studioDraft.shapes[studioShapeIndex].opacity + 0.05F); break;
    case 138: if (!studioDraft.shapes.empty() && studioDraft.shapes.size() < 64) { auto copy = studioDraft.shapes[studioShapeIndex]; copy.x = std::min(2.0F, copy.x + 0.03F); copy.y = std::min(2.0F, copy.y + 0.03F); studioDraft.shapes.insert(studioDraft.shapes.begin() + studioShapeIndex + 1, copy); ++studioShapeIndex; } break;
    case 139: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].flipHorizontal = !studioDraft.shapes[studioShapeIndex].flipHorizontal; break;
    case 146: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].flipVertical = !studioDraft.shapes[studioShapeIndex].flipVertical; break;
    case 147: if (!studioDraft.shapes.empty()) { studioDraft.shapes.erase(studioDraft.shapes.begin() + studioShapeIndex); if (!studioDraft.shapes.empty()) studioShapeIndex = std::min(studioShapeIndex, studioDraft.shapes.size() - 1); else studioShapeFocused = false; } break;
    case 148: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].overScreens = !studioDraft.shapes[studioShapeIndex].overScreens; break;
    case 149: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].overPanels = !studioDraft.shapes[studioShapeIndex].overPanels; break;
    default:
        if (action >= 60 && action <= 67) StepColorChannel(static_cast<int>(action - 60));
        break;
    }
    if (action >= 110 && action <= 149) {
        const int focused = studioShapeFocused && !studioDraft.shapes.empty()
            ? static_cast<int>(studioShapeIndex) + 1
            : studioImageFocused && !studioDraft.images.empty()
                ? -(static_cast<int>(studioImageIndex) + 1) : 0;
        try { host.FocusSkinElement(focused); } catch (...) {}
    }
    PushPreview();
}


} // namespace rivan::ui
