// Win32Ui.Settings.cpp
// Settings rendering and actions for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {
namespace {

constexpr float kSettingsCategoryGap = 7.0F;
constexpr float kSettingsSectionGap = 10.0F;
constexpr float kSettingsSectionPadding = 6.0F;

std::wstring YoutubeGrabberHotkeyLabel(std::uint32_t modifiers,
                                       std::uint32_t virtualKey) {
    std::wstring label;
    if ((modifiers & MOD_CONTROL) != 0u) label += L"CTRL + ";
    if ((modifiers & MOD_SHIFT) != 0u) label += L"SHIFT + ";
    if ((modifiers & MOD_ALT) != 0u) label += L"ALT + ";
    if ((modifiers & MOD_WIN) != 0u) label += L"WIN + ";
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    wchar_t keyName[64]{};
    const LONG keyNameData = static_cast<LONG>(scanCode << 16u);
    if (scanCode != 0 && GetKeyNameTextW(keyNameData, keyName,
                                         static_cast<int>(std::size(keyName))) > 0) {
        label += keyName;
    } else if (virtualKey >= L'A' && virtualKey <= L'Z') {
        label.push_back(static_cast<wchar_t>(virtualKey));
    } else {
        label += L"KEY " + std::to_wstring(virtualKey);
    }
    return label;
}

} // namespace

void Win32Ui::Impl::DrawSettings(const D2D1_SIZE_F size,
                                 std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    hits.clear();
    const auto panel = Rect(10.0F, 10.0F, size.width - 10.0F, size.height - 10.0F);
    target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
    auto content = DrawPanel(panel, L"RIVAN PREFERENCES", b[1].Get(), b[2].Get(), b[3].Get(),
                             b[4].Get(), b[13].Get(), b[7].Get());
    const float navigationWidth = std::clamp(Width(content) * 0.24F, 145.0F, 230.0F);
    const auto navigation = Rect(content.left + 3, content.top + 31,
                                 content.left + navigationWidth, content.bottom - 3);
    DrawBevel(navigation, b[5].Get(), b[3].Get(), b[4].Get(), true);
    const std::array categories{SettingCategory::General, SettingCategory::Library,
                                SettingCategory::Modules, SettingCategory::Appearance,
                                SettingCategory::Integrations, SettingCategory::SkinManager};
    const float categoryLeft = navigation.left;
    const float categoryRight = navigation.right;
    const float categoryTop = navigation.top + 5;
    float top = categoryTop;
    for (const auto category : categories) {
        if (top > categoryTop) {
            target->FillRectangle(Rect(categoryLeft, top - kSettingsCategoryGap,
                                       categoryRight, top), b[0].Get());
        }
        const auto row = Rect(categoryLeft, top, categoryRight, top + 25);
        const bool selected = category == model.settingsCategory;
        if (selected) target->FillRectangle(row, b[11].Get());
        DrawText(CategoryName(category), Rect(row.left + 6, row.top, row.right - 4, row.bottom),
                 selected ? b[12].Get() : b[6].Get(), regularFormat.Get());
        AddSettingHit(row, category);
        top += 25 + kSettingsCategoryGap;
    }
    const auto details = Rect(navigation.right + 7, navigation.top, content.right - 3, content.bottom - 3);
    settingsDetailsBounds = details;
    DrawBevel(details, b[5].Get(), b[3].Get(), b[4].Get(), true);
    const bool isSkinManager = model.settingsCategory == SettingCategory::SkinManager;
    if (isSkinManager) {
        DrawText(CategoryName(model.settingsCategory), Rect(details.left + 15, details.top + 13,
                 details.right - 15, details.top + 42), b[6].Get(), headingFormat.Get());
    }
    if (model.settingsCategory == SettingCategory::SkinManager) {
        DrawSkinManagerPane(details, b);
        return;
    }
    settingsSkinListBounds = {};
    settingsSkinRows = 0;
    DrawSettingsPane(details, b);
}

void Win32Ui::Impl::DrawSettingsPane(const D2D1_RECT_F& details,
                                     std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const float left = details.left + 15;
    const float right = details.right - 15;
    const float contentTop = details.top + 15;
    const float viewportBottom = details.bottom - 4;
    const float viewportHeight = std::max(0.0F, viewportBottom - contentTop);
    const float maxScroll = std::max(0.0F, settingsContentHeight - viewportHeight);
    settingsScrollY = std::clamp(settingsScrollY, 0.0F, maxScroll);
    target->PushAxisAlignedClip(Rect(details.left, contentTop, details.right, viewportBottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
    float y = contentTop - settingsScrollY + kSettingsSectionPadding;

    switch (model.settingsCategory) {
    case SettingCategory::General:
        DrawWindowsSection(left, right, y, b);
        break;
    case SettingCategory::Library:
        DrawLibrarySection(left, right, y, b);
        break;
    case SettingCategory::Modules:
        DrawModulesSection(left, right, y, b);
        break;
    case SettingCategory::Appearance:
        DrawAppearanceSection(left, right, y, b);
        break;
    case SettingCategory::Integrations:
        DrawIntegrationsSection(left, right, y, b);
        break;
    case SettingCategory::SkinManager:
        break;
    }
    target->PopAxisAlignedClip();
    y += kSettingsSectionPadding;
    settingsContentHeight = (y + settingsScrollY) - contentTop + 8.0F;
    const float contentMax = std::max(0.0F, settingsContentHeight - viewportHeight);
    settingsScrollY = std::clamp(settingsScrollY, 0.0F, contentMax);
    if (contentMax > 0.5F) {
        const float trackLeft = details.right - 8.0F;
        const float trackTop = contentTop;
        const float trackBottom = viewportBottom;
        const float trackHeight = trackBottom - trackTop;
        DrawBevel(Rect(trackLeft, trackTop, details.right - 3.0F, trackBottom), b[5].Get(), b[3].Get(), b[4].Get(), true, 1.0F);
        const float thumbHeight = std::max(24.0F, trackHeight * (viewportHeight / settingsContentHeight));
        const float thumbY = trackTop + (settingsScrollY / contentMax) * (trackHeight - thumbHeight);
        target->FillRectangle(Rect(trackLeft + 1.0F, thumbY, details.right - 4.0F, thumbY + thumbHeight), b[8].Get());
    }
}

void Win32Ui::Impl::DrawSettingsSectionGap(float& y,
                                           std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    y += kSettingsSectionPadding;
    target->FillRectangle(Rect(settingsDetailsBounds.left, y,
                               settingsDetailsBounds.right,
                               y + kSettingsSectionGap), b[0].Get());
    y += kSettingsSectionGap + kSettingsSectionPadding;
}

void Win32Ui::Impl::DrawWindowsSection(const float left, const float right, float& y,
                                       std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    DrawText(L"WINDOWS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    const float optionWidth = (right - left - 8) * 0.5F;
    SettingsButton(Rect(left, y, left + optionWidth, y + 24),
                   model.startAtStartup ? L"START AT STARTUP: ON" : L"START AT STARTUP: OFF", 15, b);
    SettingsButton(Rect(left + optionWidth + 8, y, right, y + 24),
                   model.exitToTray ? L"EXIT TO TRAY: ON" : L"EXIT TO TRAY: OFF", 16, b);
    y += 34;
}

void Win32Ui::Impl::DrawLibrarySection(const float left, const float right, float& y,
                                       std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    // Music folder list. Show every configured root, then one empty slot so the next
    // folder can be chosen. After each choice another empty slot appears (no limit).
    // Subfolders of all roots become playlists.
    DrawText(L"MUSIC FOLDER(S)", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    const auto field = [&](const wchar_t* caption, const std::wstring& value,
                           std::size_t index, bool allowClear) {
        if (caption && *caption) {
            DrawText(caption, Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 29;
        } else {
            y += 4;
        }
        const float browseWidth = 78.0F;
        const float clearWidth = allowClear ? 60.0F : 0.0F;
        const auto box = Rect(left, y, right - browseWidth - clearWidth - 8, y + 24);
        DrawBevel(box, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        DrawText(value.empty() ? L"(not set)" : value, Rect(box.left + 5, box.top, box.right - 4, box.bottom),
                 value.empty() ? b[10].Get() : b[6].Get(), regularFormat.Get());
        SettingsButton(Rect(right - browseWidth - clearWidth - 4, y,
                            right - clearWidth - 4, y + 24), L"BROWSE...", 100 + index, b);
        if (allowClear) SettingsButton(Rect(right - clearWidth, y, right, y + 24), L"CLEAR", 200 + index, b);
        y += 28;
    };
    const std::wstring primary = model.musicFolders.empty() ? std::wstring{} : model.musicFolders[0];
    field(nullptr, primary, 0, false);
    if (!primary.empty()) {
        for (std::size_t i = 1; i < model.musicFolders.size(); ++i) {
            field(nullptr, model.musicFolders[i], i, true);
        }
        field(nullptr, L"", model.musicFolders.size(), false);
    }
    y += 4;
    DrawSettingsSectionGap(y, b);
    DrawText(L"FILE PREVIEW", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    SettingsButton(Rect(left, y, right, y + 24),
                   model.filePreviewEnabled ? L"FILE PREVIEW: ON" : L"FILE PREVIEW: OFF", 14, b);
    y += 26;
    SettingsButton(Rect(left, y, right, y + 24),
                   model.previewFitWindow ? L"FIT WINDOW TO VIDEO: ON" : L"FIT WINDOW TO VIDEO: OFF", 25, b);
    y += 26;
    DrawText(model.previewFitWindow ? L"Preview fullscreen grows the window to remove black bars."
                                    : L"Preview fullscreen keeps the current window size.",
             Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 16;
    DrawSettingsSectionGap(y, b);
    DrawText(L"PLAYLIST DUPLICATE", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    SettingsButton(Rect(left, y, right, y + 24),
                   model.duplicateAsFile ? L"DUPLICATE: COPY FILE ON DISK" : L"DUPLICATE: ADD SECOND ENTRY", 17, b);
    y += 26;
    DrawText(model.duplicateAsFile ? L"Right-click > Duplicate copies the audio file and adds the copy."
                                  : L"Right-click > Duplicate adds another reference to the same track.",
             Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 22;
}

void Win32Ui::Impl::DrawModulesSection(const float left, const float right, float& y,
                                       std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    DrawText(L"MODULE VISIBILITY", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    const auto moduleLabel = [this](ModuleId id) {
        const auto* item = model.moduleLayout.Find(id);
        std::wstring label(UiModuleRegistry::Get(id).Title());
        label += item && item->visible ? L": ON" : L": OFF";
        return label;
    };
    constexpr std::array moduleIds{ModuleId::Rivan, ModuleId::AllMusic,
                                    ModuleId::GraphicEqualizer, ModuleId::RivanLibrary,
                                    ModuleId::VideoPreview, ModuleId::Lyrics};
    for (std::size_t i = 0; i < moduleIds.size(); i += 2) {
        const float optionWidth = (right - left - 8.0F) * 0.5F;
        SettingsButton(Rect(left, y, left + optionWidth, y + 24), moduleLabel(moduleIds[i]), 60 + i, b);
        if (i + 1 < moduleIds.size()) {
            SettingsButton(Rect(left + optionWidth + 8, y, right, y + 24), moduleLabel(moduleIds[i + 1]), 60 + i + 1, b);
        }
        y += 30;
    }
    SettingsButton(Rect(left, y, right, y + 24), L"RESET MODULE LAYOUT", 66, b);
    y += 34;
    DrawText(L"Drag a title to move. Center drops create tabs; side drops snap.", Rect(left, y, right, y + 28), b[6].Get(), tinyFormat.Get());
    y += 30;
    DrawSettingsSectionGap(y, b);
    DrawText(L"EXPANDING BEHAVIOR ON NO SPACE", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    SettingsButton(Rect(left, y, right, y + 24),
                   model.moduleExpansionBehavior == ModuleExpansionBehavior::Squash ? L"ON EXPAND: SQUASH" : L"ON EXPAND: RESIZE", 67, b);
    y += 26;
    DrawText(model.moduleExpansionBehavior == ModuleExpansionBehavior::Squash
                 ? L"Shrinks other modules to make room."
                 : L"Grows window, then restores size after collapse.",
              Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 16;
    DrawSettingsSectionGap(y, b);
    DrawText(L"MODULE RESIZE COLLISIONS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    SettingsButton(Rect(left, y, right, y + 24),
                   model.moduleResizeBehavior == ModuleResizeBehavior::Squash
                       ? L"RESIZE: SQUASH OCCUPIED" : L"RESIZE: ALLOW OVERLAP", 69, b);
    y += 26;
    DrawText(model.moduleResizeBehavior == ModuleResizeBehavior::Squash
                 ? L"The resized module takes space from modules in its path."
                 : L"The resized module may overlap other modules.",
             Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 22;
}

void Win32Ui::Impl::DrawAppearanceSection(const float left, const float right, float& y,
                                          std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    DrawText(L"WINDOW RESIZE BEHAVIOR", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    SettingsButton(Rect(left, y, right, y + 24),
                   model.windowResizeBehavior == WindowResizeBehavior::ScaleAll
                       ? L"RESIZE: SCALE ALL" : L"RESIZE: GROW TRAILING MODULE", 68, b);
    y += 26;
    DrawText(model.windowResizeBehavior == WindowResizeBehavior::ScaleAll
                 ? L"All modules keep their proportions as the window changes."
                 : L"The module touching the resized edge absorbs available space.",
             Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 16;
    DrawSettingsSectionGap(y, b);
    DrawSongRowLayoutEditor(left, right, y, b);
}

void Win32Ui::Impl::DrawIntegrationsSection(const float left, const float right, float& y,
                                            std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    DrawText(L"DISCORD", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    SettingsButton(Rect(left, y, right, y + 24), model.discordEnabled ? L"RICH PRESENCE: ON" : L"RICH PRESENCE: OFF", 18, b);
    y += 26;
    DrawText(L"Shows the playing track in Discord. Needs Discord desktop running.", Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 20;
    const float optionWidth = (right - left - 8) * 0.5F;
    SettingsButton(Rect(left, y, left + optionWidth, y + 24), model.discordShowArtist ? L"SHOW ARTIST: ON" : L"SHOW ARTIST: OFF", 20, b);
    SettingsButton(Rect(left + optionWidth + 8, y, right, y + 24), model.discordShowImageText ? L"IMAGE TEXT: RIVAN" : L"IMAGE TEXT: OFF", 21, b);
    y += 34;
    SettingsButton(Rect(left, y, right, y + 24), model.discordShowGithubButton ? L"GITHUB BUTTON: ON" : L"GITHUB BUTTON: OFF", 23, b);
    y += 26;
    DrawText(L"Visible to other users only; links to https://github.com/gyatstian/Rivan.", Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 16;
    DrawSettingsSectionGap(y, b);
    DrawText(L"YOUTUBE", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    SettingsButton(Rect(left, y, right, y + 24), model.youtubeEnabled ? L"YOUTUBE DOWNLOADER: ON" : L"YOUTUBE DOWNLOADER: OFF", 4, b);
    y += 30;
    std::wstring hotkeyLabel = youtubeGrabberHotkeyCapture
        ? (youtubeGrabberHotkeyCaptureFailed ? L"HOTKEY UNAVAILABLE — TRY ANOTHER" : L"PRESS A SHORTCUT...")
        : L"LINK GRABBER HOTKEY: " +
              YoutubeGrabberHotkeyLabel(model.youtubeGrabberHotkeyModifiers,
                                        model.youtubeGrabberHotkeyVirtualKey);
    if (!youtubeGrabberHotkeyCapture && model.youtubeEnabled &&
        !model.youtubeGrabberHotkeyAvailable) {
        hotkeyLabel += L" (UNAVAILABLE)";
    }
    SettingsButton(Rect(left, y, right, y + 24), hotkeyLabel, 24, b);
    y += 26;
    DrawText(model.youtubeGrabberHotkeyAvailable || !model.youtubeEnabled
                 ? L"Uses the active browser address. Works while Rivan is minimized or in tray."
                 : L"Click to choose an available global shortcut.",
             Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 22;
    const bool showYtInstall = !model.youtubeYtDlpInstalled || model.youtubeInstallingYtDlp;
    const bool showFfInstall = !model.youtubeFfmpegInstalled || model.youtubeInstallingFfmpeg;
    if (showYtInstall || showFfInstall) {
        const wchar_t* yt = model.youtubeInstallingYtDlp ? L"INSTALLING YT-DLP..." : L"INSTALL YT-DLP";
        const wchar_t* ff = model.youtubeInstallingFfmpeg ? L"INSTALLING FFMPEG..." : L"INSTALL FFMPEG";
        if (showYtInstall && showFfInstall) {
            const float width = (right - left - 8) * 0.5F;
            SettingsButton(Rect(left, y, left + width, y + 24), yt, 5, b);
            SettingsButton(Rect(left + width + 8, y, right, y + 24), ff, 6, b);
        } else SettingsButton(Rect(left, y, right, y + 24), showYtInstall ? yt : ff, showYtInstall ? 5 : 6, b);
        y += 28;
    }
    DrawSettingsSectionGap(y, b);
    DrawText(L"LYRICS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 29;
    SettingsButton(Rect(left, y, right, y + 24),
                   model.lyricsCacheEnabled ? L"SAVE FETCHED LYRICS: ON" : L"SAVE FETCHED LYRICS: OFF", 7, b);
    y += 26;
    DrawText(L"Stores fetched lyrics locally for offline retrieval.", Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
    y += 22;
}

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

void Win32Ui::Impl::DrawSkinManagerPane(const D2D1_RECT_F& details,
                                        std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const float left = details.left + 15;
    const float right = details.right - 15;
    const auto buttons = Rect(left, details.top + 46, right, details.top + 72);
    const float half = Width(buttons) * 0.5F;
    StudioButton(Rect(buttons.left, buttons.top, buttons.left + half - 4, buttons.bottom), L"SKIN STUDIO", 100, b);
    StudioButton(Rect(buttons.left + half + 4, buttons.top, buttons.right, buttons.bottom), L"SKIN FOLDER", 101, b);
    DrawText(L"SAVED SKINS", Rect(left, buttons.bottom + 6, right, buttons.bottom + 22), b[8].Get(), tinyFormat.Get());
    const auto list = Rect(left, buttons.bottom + 24, right, details.bottom - 8);
    settingsSkinListBounds = list;
    settingsSkinRows = static_cast<std::size_t>(std::max(1.0F, std::floor((Height(list) - 4.0F) / 29.0F)));
    const std::size_t maximum = model.skins.size() > settingsSkinRows ? model.skins.size() - settingsSkinRows : 0;
    settingsSkinScroll = std::min(settingsSkinScroll, maximum);
    DrawBevel(list, b[5].Get(), b[3].Get(), b[4].Get(), true);
    target->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_ALIASED);
    float rowTop = list.top + 2;
    const std::size_t last = std::min(model.skins.size(), settingsSkinScroll + settingsSkinRows);
    for (std::size_t index = settingsSkinScroll; index < last; ++index) {
        const auto& skin = model.skins[index];
        const auto row = Rect(list.left + 2, rowTop, list.right - 2, rowTop + 27);
        const bool hot = Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        if (skin.active) target->FillRectangle(row, b[11].Get());
        else if (hot) target->FillRectangle(row, b[7].Get());
        DrawText(skin.active ? L"\u25B6" : (skin.builtIn ? L"\u2302" : L"\u2022"), Rect(row.left + 4, row.top, row.left + 20, row.bottom), skin.active ? b[12].Get() : b[6].Get(), regularFormat.Get());
        const float actionsLeft = skin.builtIn ? row.right : row.right - 150.0F;
        DrawText(skin.name, Rect(row.left + 22, row.top, actionsLeft - 4, row.bottom), skin.active ? b[12].Get() : b[9].Get(), regularFormat.Get());
        if (!skin.builtIn) {
            StudioButton(Rect(actionsLeft, row.top + 3, actionsLeft + 46, row.bottom - 3), L"RENAME", 600 + index, b);
            StudioButton(Rect(actionsLeft + 50, row.top + 3, actionsLeft + 96, row.bottom - 3), L"EDIT", 700 + index, b);
            StudioButton(Rect(actionsLeft + 100, row.top + 3, row.right - 2, row.bottom - 3), L"DELETE", 800 + index, b);
        }
        HitRegion hit;
        hit.bounds = Rect(row.left, row.top, actionsLeft, row.bottom);
        hit.kind = HitKind::Studio;
        hit.id = 200 + index;
        hits.push_back(hit);
        rowTop += 29;
    }
    target->PopAxisAlignedClip();
    if (managerNameEditing && managerSkinIndex < model.skins.size()) {
        const auto prompt = Rect(list.left + 8, list.bottom - 34, list.right - 8, list.bottom - 7);
        target->FillRectangle(prompt, b[1].Get());
        DrawBevel(Rect(prompt.left, prompt.top, prompt.right - 58, prompt.bottom), b[5].Get(), b[3].Get(), b[4].Get(), true);
        DrawText(managerSkinName + (((GetTickCount64() / 500ULL) % 2ULL == 0ULL) ? L"_" : L""),
                 Rect(prompt.left + 5, prompt.top, prompt.right - 63, prompt.bottom), b[9].Get(), regularFormat.Get());
        StudioButton(Rect(prompt.right - 54, prompt.top, prompt.right, prompt.bottom), L"APPLY", 900 + managerSkinIndex, b);
    }
    if (model.skins.empty()) DrawText(L"< NO SKINS INSTALLED >", list, b[10].Get(), regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Win32Ui::Impl::SettingsButton(const D2D1_RECT_F& bounds, const std::wstring& label,
                                   std::uint64_t action,
                                   std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const bool isSkinManager = model.settingsCategory == SettingCategory::SkinManager;
    const float clipTop = settingsDetailsBounds.top + (isSkinManager ? 50.0F : 15.0F);
    const float clipBottom = settingsDetailsBounds.bottom - 4.0F;
    const bool inViewport = windowKind != WindowKind::Settings ||
                            (bounds.bottom > clipTop && bounds.top < clipBottom);
    D2D1_RECT_F hitBounds = bounds;
    if (windowKind == WindowKind::Settings && inViewport) {
        hitBounds.top = std::max(bounds.top, clipTop);
        hitBounds.bottom = std::min(bounds.bottom, clipBottom);
    }
    const bool hot = inViewport && Contains(hitBounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
    DrawBevel(bounds, hot ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(), false);
    DrawText(label, bounds, b[9].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!inViewport || hitBounds.bottom <= hitBounds.top) return;
    HitRegion hit;
    hit.bounds = hitBounds;
    hit.kind = HitKind::SettingsAction;
    hit.id = action;
    hits.push_back(hit);
}

void Win32Ui::Impl::HandleSettingsAction(std::uint64_t action) {
    try {
        const auto updateSongRow = [this](const auto& update) {
            if (!songRowEditorVisible) return;
            update(songRowDraft);
            CommitSongRowLayoutEdit();
        };
        if (action >= 100 && action < 200) {
            if (auto folder = PickFolder()) host.SetMusicFolder(static_cast<std::size_t>(action - 100), *folder);
            return;
        }
        if (action >= 200 && action < 300) {
            host.SetMusicFolder(static_cast<std::size_t>(action - 200), std::filesystem::path{});
            return;
        }
        switch (action) {
        case 4: host.SetYoutubeEnabled(!model.youtubeEnabled); break;
        case 5: if (!model.youtubeYtDlpInstalled && !model.youtubeInstallingYtDlp) host.InstallYoutubeTool(true); break;
        case 6: if (!model.youtubeFfmpegInstalled && !model.youtubeInstallingFfmpeg) host.InstallYoutubeTool(false); break;
        case 7: host.SetLyricsCacheEnabled(!model.lyricsCacheEnabled); break;
        case 14: host.SetFilePreviewEnabled(!model.filePreviewEnabled); break;
        case 25: host.SetPreviewFitWindow(!model.previewFitWindow); break;
        case 15: host.SetStartAtStartup(!model.startAtStartup); break;
        case 16: host.SetExitToTray(!model.exitToTray); break;
        case 17: host.SetDuplicateAsFile(!model.duplicateAsFile); break;
        case 18: host.SetDiscordEnabled(!model.discordEnabled); break;
        case 20: host.SetDiscordShowArtist(!model.discordShowArtist); break;
        case 21: host.SetDiscordShowImageText(!model.discordShowImageText); break;
        case 70:
            songRowEditorVisible = true;
            songRowDraft = model.songRowLayout;
            songRowSelectedField.reset();
            break;
        case 71:
            updateSongRow([](SongRowLayout& layout) { layout = SongRowLayout::Defaults(); });
            songRowSelectedField.reset();
            break;
        case 72:
            updateSongRow([this](SongRowLayout& layout) {
                if (!songRowSelectedField) return;
                const auto hidden = *songRowSelectedField;
                layout.Field(hidden).visible = false;
                layout.Field(hidden).snap.reset();
                for (auto& field : layout.fields) {
                    if (field.snap && field.snap->target == hidden) field.snap.reset();
                }
            });
            songRowSnap.reset();
            songRowSelectedField.reset();
            break;
        case 73:
            updateSongRow([this](SongRowLayout& layout) {
                if (songRowSelectedField) {
                    auto& field = layout.Field(*songRowSelectedField);
                    field.fontSizeDelta = std::max(-8, field.fontSizeDelta - 1);
                }
            });
            break;
        case 74:
            updateSongRow([this](SongRowLayout& layout) {
                if (songRowSelectedField) {
                    auto& field = layout.Field(*songRowSelectedField);
                    field.fontSizeDelta = std::min(16, field.fontSizeDelta + 1);
                }
            });
            break;
        case 76:
            updateSongRow([this](SongRowLayout& layout) {
                if (!songRowSelectedField) return;
                auto& weight = layout.Field(*songRowSelectedField).fontWeight;
                weight = weight == SongRowFontWeight::Normal ? SongRowFontWeight::SemiBold
                    : weight == SongRowFontWeight::SemiBold ? SongRowFontWeight::Bold
                                                            : SongRowFontWeight::Normal;
            });
            break;
        case 77:
            updateSongRow([this](SongRowLayout& layout) {
                if (!songRowSelectedField) return;
                auto& color = layout.Field(*songRowSelectedField).textColor;
                color = color == SongRowTextColor::Primary ? SongRowTextColor::Secondary
                                                            : SongRowTextColor::Primary;
            });
            break;
        case 80:
            updateSongRow([](SongRowLayout& layout) {
                layout.rowHeight = std::max(20.0F, layout.rowHeight - 4.0F);
            });
            break;
        case 81:
            updateSongRow([](SongRowLayout& layout) {
                layout.rowHeight = std::min(160.0F, layout.rowHeight + 4.0F);
            });
            break;
        case 82:
            updateSongRow([this](SongRowLayout& layout) {
                if (!songRowSelectedField) return;
                auto& style = layout.Field(*songRowSelectedField).fontStyle;
                style = style == SongRowFontStyle::Normal ? SongRowFontStyle::Italic
                                                           : SongRowFontStyle::Normal;
            });
            break;
        case 83:
            AdjustSongRowSnapGap(1);
            break;
        case 84:
            AdjustSongRowSnapGap(-1);
            break;
        case 23: host.SetDiscordShowGithubButton(!model.discordShowGithubButton); break;
        case 24:
            youtubeGrabberHotkeyCapture = true;
            youtubeGrabberHotkeyCaptureFailed = false;
            break;
        case 60: case 61: case 62: case 63: case 64: case 65: {
            auto layout = model.moduleLayout;
            const auto id = static_cast<ModuleId>(action - 60);
            if (auto* item = layout.Find(id)) {
                item->visible = !item->visible;
                if (!item->visible && layout.IsTabbed(id)) {
                    layout.RemoveTab(id);
                }
                host.SetModuleLayout(layout);
            }
            break;
        }
        case 66: host.SetModuleLayout(ModuleLayout::Defaults()); break;
        case 67: host.SetModuleExpansionBehavior(model.moduleExpansionBehavior == ModuleExpansionBehavior::Squash
                                                       ? ModuleExpansionBehavior::Resize : ModuleExpansionBehavior::Squash); break;
        case 68: host.SetWindowResizeBehavior(model.windowResizeBehavior == WindowResizeBehavior::ScaleAll
                                                   ? WindowResizeBehavior::GrowTrailingModule
                                                   : WindowResizeBehavior::ScaleAll); break;
        case 69: host.SetModuleResizeBehavior(model.moduleResizeBehavior == ModuleResizeBehavior::Squash
                                                   ? ModuleResizeBehavior::Overlap
                                                   : ModuleResizeBehavior::Squash); break;
        case 50: if (window) KillTimer(window, kYoutubeSearchDebounceTimer); host.SubmitYoutubeQuery(playlistQuery); break;
        case 51: if (model.youtubeCanPagePrev && model.youtubePage > 0) host.SetYoutubeSearchPage(model.youtubePage - 1); break;
        case 52: if (model.youtubeCanPageNext) host.SetYoutubeSearchPage(model.youtubePage + 1); break;
        default: break;
        }
        if (action >= 90 && action < 90 + kSongRowFieldCount) {
            updateSongRow([action](SongRowLayout& layout) {
                layout.Field(static_cast<SongRowField>(action - 90)).visible = true;
            });
            songRowSelectedField = static_cast<SongRowField>(action - 90);
        }
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

[[nodiscard]] std::optional<std::filesystem::path> Win32Ui::Impl::PickFolder() {
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.GetAddressOf())))) return std::nullopt;
    DWORD dialogOptions = 0;
    if (SUCCEEDED(dialog->GetOptions(&dialogOptions))) {
        dialog->SetOptions(dialogOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    }
    if (FAILED(dialog->Show(window))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return std::nullopt;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return std::nullopt;
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

} // namespace rivan::ui
