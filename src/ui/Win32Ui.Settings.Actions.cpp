// Win32Ui.Settings.Actions.cpp
// Settings action handling for Win32Ui::Impl.
#include "Win32UiImpl.h"
#include "../core/AppPaths.h"

namespace rivan::ui {

void Win32Ui::Impl::HandleSettingsAction(std::uint64_t action) {
    try {
        if (action == 27) { statisticsPeriodDropdown = !statisticsPeriodDropdown; return; }
        constexpr std::array periodOptions = {
            stats::DashboardPeriod::Week,
            stats::DashboardPeriod::FourWeeks,
            stats::DashboardPeriod::Month,
            stats::DashboardPeriod::SixMonths,
            stats::DashboardPeriod::Year,
            stats::DashboardPeriod::AllTime
        };
        if (action >= 320 && action <= 325) {
            statisticsPeriodDropdown = false;
            host.SetStatisticsPeriod(periodOptions[action - 320]);
            return;
        }
        statisticsPeriodDropdown = false;
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
        case 5: if (!model.youtubeYtDlpInstalled && !model.youtubeInstallingYtDlp) host.InstallYoutubeTool(youtube::YoutubeTool::YtDlp); break;
        case 6: if (!model.youtubeFfmpegInstalled && !model.youtubeInstallingFfmpeg) host.InstallYoutubeTool(youtube::YoutubeTool::Ffmpeg); break;
        case 8: if (!model.youtubeDenoInstalled && !model.youtubeInstallingDeno) host.InstallYoutubeTool(youtube::YoutubeTool::Deno); break;
        case 7: host.SetLyricsCacheEnabled(!model.lyricsCacheEnabled); break;
        case 29: host.SetLyricsOnlineEnabled(!model.lyricsOnlineEnabled); break;
        case 30: host.SetLyricsFakeTimestampsEnabled(!model.lyricsFakeTimestampsEnabled); break;
        case 28: {
            using AT = config::LyricsTextAlignment;
            const AT current = model.lyricsAlignment;
            const AT next = current == AT::Left ? AT::Center
                           : current == AT::Center ? AT::Right
                                                   : AT::Left;
            host.SetLyricsAlignment(next);
            break;
        }
        case 26: host.SetStatsEnabled(!model.statsEnabled); break;
        case 300: host.SetStatisticsTracksExpanded(!model.statistics.tracksExpanded); break;
        case 301: if (!model.statistics.tracksExpanded && model.statistics.tracksPage > 0) host.SetStatisticsTracksPage(model.statistics.tracksPage - 1); break;
        case 302: if (!model.statistics.tracksExpanded) host.SetStatisticsTracksPage(model.statistics.tracksPage + 1); break;
        case 310: host.SetStatisticsArtistsExpanded(!model.statistics.artistsExpanded); break;
        case 311: if (!model.statistics.artistsExpanded && model.statistics.artistsPage > 0) host.SetStatisticsArtistsPage(model.statistics.artistsPage - 1); break;
        case 312: if (!model.statistics.artistsExpanded) host.SetStatisticsArtistsPage(model.statistics.artistsPage + 1); break;
        case 14: host.SetFilePreviewEnabled(!model.filePreviewEnabled); break;
        case 25: host.SetPreviewFitWindow(!model.previewFitWindow); break;
        case 15: host.SetStartAtStartup(!model.startAtStartup); break;
        case 16: host.SetExitToTray(!model.exitToTray); break;
        case 31: {
            const auto dataRoot = core::AppPaths::LocalDataRoot();
            if (!dataRoot.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(dataRoot, ec);
                ShellExecuteW(window, L"open", dataRoot.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case 17: host.SetDuplicateAsFile(!model.duplicateAsFile); break;
        case 18: host.SetDiscordEnabled(!model.discordEnabled); break;
        case 20: host.SetDiscordShowArtist(!model.discordShowArtist); break;
        case 21: {
            using DST = config::DiscordSecondaryText;
            DST current = model.discordSecondaryText;
            DST next = (current == DST::Off) ? DST::SyncLyrics
                       : (current == DST::SyncLyrics) ? DST::TotalStreams
                                                      : DST::Off;
            host.SetDiscordSecondaryText(next);
            break;
        }
        case 22:
            if (model.discordSecondaryText == config::DiscordSecondaryText::SyncLyrics) {
                host.SetDiscordFallbackToTotalStreams(!model.discordFallbackToTotalStreams);
            }
            break;
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
