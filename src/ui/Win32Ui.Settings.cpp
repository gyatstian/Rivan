// Win32Ui.Settings.cpp
// Settings rendering and actions for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {
namespace {

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
    const std::array categories{SettingCategory::General, SettingCategory::Appearance,
                                SettingCategory::Discord, SettingCategory::Online,
                                SettingCategory::SkinManager};
    float top = navigation.top + 5;
    for (const auto category : categories) {
        const auto row = Rect(navigation.left + 4, top, navigation.right - 4, top + 25);
        const bool selected = category == model.settingsCategory;
        if (selected) target->FillRectangle(row, b[11].Get());
        DrawText(CategoryName(category), Rect(row.left + 6, row.top, row.right - 4, row.bottom),
                 selected ? b[12].Get() : b[6].Get(), regularFormat.Get());
        AddSettingHit(row, category);
        top += 27;
    }
    const auto details = Rect(navigation.right + 7, navigation.top, content.right - 3, content.bottom - 3);
    settingsDetailsBounds = details;
    DrawBevel(details, b[5].Get(), b[3].Get(), b[4].Get(), true);
    const bool integrationCategory = model.settingsCategory == SettingCategory::General ||
                                     model.settingsCategory == SettingCategory::Appearance ||
                                     model.settingsCategory == SettingCategory::Discord ||
                                     model.settingsCategory == SettingCategory::Online;
    if (!integrationCategory) {
        DrawText(CategoryName(model.settingsCategory), Rect(details.left + 15, details.top + 13,
                 details.right - 15, details.top + 42), b[6].Get(), headingFormat.Get());
    }
    if (model.settingsCategory == SettingCategory::SkinManager) {
        DrawSkinManagerPane(details, b);
        return;
    }
    settingsSkinListBounds = {};
    settingsSkinRows = 0;
    if (integrationCategory) {
        DrawGeneralPane(details, b);
        return;
    }
    settingsScrollY = 0.0F;
    settingsContentHeight = 0.0F;
    DrawText(L"Settings values and persistence are owned by the application core.",
             Rect(details.left + 15, details.top + 53, details.right - 15, details.top + 78),
             b[9].Get(), regularFormat.Get());
    DrawText(L"This classic control surface keeps the existing validated callback seam.",
             Rect(details.left + 15, details.top + 79, details.right - 15, details.top + 105),
             b[10].Get(), smallFormat.Get());
    DrawBevel(Rect(details.left + 15, details.bottom - 48, details.right - 15, details.bottom - 16),
              b[1].Get(), b[3].Get(), b[4].Get());
    DrawText(L"CALLBACK INTERFACE: ONLINE", Rect(details.left + 20, details.bottom - 48,
             details.right - 20, details.bottom - 16), b[8].Get(), regularFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Win32Ui::Impl::DrawGeneralPane(const D2D1_RECT_F& details,
                                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const float left = details.left + 15;
    const float right = details.right - 15;
    const float contentTop = details.top + 15;
    const float viewportBottom = details.bottom - 4;
    const float viewportHeight = std::max(0.0F, viewportBottom - contentTop);
    const float maxScroll = std::max(0.0F, settingsContentHeight - viewportHeight);
    settingsScrollY = std::clamp(settingsScrollY, 0.0F, maxScroll);
    target->PushAxisAlignedClip(Rect(details.left + 2, contentTop, details.right - 2, viewportBottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
    float y = contentTop - settingsScrollY;

    if (model.settingsCategory == SettingCategory::General) {
        DrawText(L"WINDOWS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        const float optionWidth = (right - left - 8) * 0.5F;
        SettingsButton(Rect(left, y, left + optionWidth, y + 24),
                       model.startAtStartup ? L"START AT STARTUP: ON" : L"START AT STARTUP: OFF", 15, b);
        SettingsButton(Rect(left + optionWidth + 8, y, right, y + 24),
                       model.exitToTray ? L"EXIT TO TRAY: ON" : L"EXIT TO TRAY: OFF", 16, b);
        y += 34;

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
        field(L"MUSIC FOLDER", primary, 0, false);
        if (!primary.empty()) {
            for (std::size_t i = 1; i < model.musicFolders.size(); ++i) {
                field(L"", model.musicFolders[i], i, true);
            }
            field(L"", L"", model.musicFolders.size(), false);
        }
        y += 10;
        DrawText(L"FILE PREVIEW", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.filePreviewEnabled ? L"FILE PREVIEW: ON" : L"FILE PREVIEW: OFF", 14, b);
        y += 34;
        DrawText(L"PLAYLISTS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
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

    if (model.settingsCategory == SettingCategory::Appearance) {
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
        y += 36;
        DrawText(L"EXPANDING BEHAVIOR ON NO SPACE", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.moduleExpansionBehavior == ModuleExpansionBehavior::Squash ? L"ON EXPAND: SQUASH" : L"ON EXPAND: RESIZE", 67, b);
        y += 26;
        DrawText(model.moduleExpansionBehavior == ModuleExpansionBehavior::Squash
                     ? L"Shrinks other modules to make room."
                     : L"Grows window, then restores size after collapse.",
                  Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;
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
        y += 22;
        DrawText(L"TRACK COVERS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24), model.trackCoverArtEnabled ? L"SONG COVERS: ON" : L"SONG COVERS: OFF", 22, b);
        y += 26;
        DrawText(L"Small cached covers appear after titles when embedded artwork is available.", Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;
    }

    if (model.settingsCategory == SettingCategory::Discord) {
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
        y += 22;
    }

    if (model.settingsCategory == SettingCategory::Online) {
        DrawText(L"LYRICS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.lyricsCacheEnabled ? L"SAVE FETCHED LYRICS: ON" : L"SAVE FETCHED LYRICS: OFF", 7, b);
        y += 26;
        DrawText(L"Stores fetched lyrics locally for offline retrieval.", Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 28;
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
    }
    target->PopAxisAlignedClip();
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
    const bool integrationCategory = model.settingsCategory == SettingCategory::General ||
                                     model.settingsCategory == SettingCategory::Appearance ||
                                     model.settingsCategory == SettingCategory::Discord ||
                                     model.settingsCategory == SettingCategory::Online;
    const float clipTop = settingsDetailsBounds.top + (integrationCategory ? 15.0F : 50.0F);
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
        case 15: host.SetStartAtStartup(!model.startAtStartup); break;
        case 16: host.SetExitToTray(!model.exitToTray); break;
        case 17: host.SetDuplicateAsFile(!model.duplicateAsFile); break;
        case 18: host.SetDiscordEnabled(!model.discordEnabled); break;
        case 20: host.SetDiscordShowArtist(!model.discordShowArtist); break;
        case 21: host.SetDiscordShowImageText(!model.discordShowImageText); break;
        case 22: host.SetTrackCoverArtEnabled(!model.trackCoverArtEnabled); break;
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
                    if (layout.tabCount < 2) layout.ClearTabs();
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
        case 50: if (window) KillTimer(window, kYoutubeSearchDebounceTimer); host.SubmitYoutubeQuery(playlistQuery); break;
        case 51: if (model.youtubeCanPagePrev && model.youtubePage > 0) host.SetYoutubeSearchPage(model.youtubePage - 1); break;
        case 52: if (model.youtubeCanPageNext) host.SetYoutubeSearchPage(model.youtubePage + 1); break;
        default: break;
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
