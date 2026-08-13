// AllMusicModule.cpp
// Rendering for the ALL MUSIC playlist-editor module.
#include "../ui/Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::DrawPlaylistEditor(const D2D1_RECT_F& bounds,
                             std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        auto content = DrawPanel(bounds, SelectedPlaylistName(), b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get(), ModuleId::AllMusic);
        const auto controls = Rect(content.left + 2, std::max(content.top, content.bottom - 28),
                                   content.right - 2, content.bottom - 2);
        playlistListBounds = Rect(content.left + 2, content.top + 2, content.right - 2,
                                  std::max(content.top + 2, controls.top - 2));
        auto tracks = Filtered(model.tracks, L"");
        // SCREEN: Playlist Editor track list.
        Win32Ui::Impl::DrawTrackRows(playlistListBounds, tracks, playlistScroll, playlistRows, b[5].Get(), b[6].Get(),
                       b[6].Get(), b[11].Get(), b[12].Get());

        // ADD imports files into user playlists and library folders. REM removes entries
        // only from user playlists.
        // Hits always register so a click on a grayed button can show the hint dialog
        // instead of feeling dead; handlers gate the real work.
        const bool editable = model.selectedPlaylistIsUser;
        const float buttonW = 42.0F;
        float x = controls.left;
        const auto editorButton = [&](const wchar_t* label, HitKind kind, bool enabled) {
            const auto rect = Rect(x, controls.top + 2, x + buttonW, controls.bottom);
            const bool hot =
                Contains(rect, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            Win32Ui::Impl::DrawBevel(rect, (enabled && hot) ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get());
            Win32Ui::Impl::DrawText(label, rect, enabled ? b[9].Get() : b[10].Get(), tinyFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            Win32Ui::Impl::AddSimpleHit(rect, kind);
            x += buttonW + 3;
        };
        editorButton(L"ADD", HitKind::EditorAdd, model.selectedPlaylistCanAdd);
        editorButton(L"REM", HitKind::EditorRemove, editable && !trackSelection.empty());
        if (cachedPlaylistDurationRevision != model.revision) {
            cachedPlaylistDurationRevision = model.revision;
            cachedPlaylistDuration = 0.0;
            for (const auto& track : model.tracks) {
                cachedPlaylistDuration += std::max(0.0, track.durationSeconds);
            }
        }
        const std::wstring status = FormatTime(model.positionSeconds) + L" / " +
                                    FormatTime(cachedPlaylistDuration);
        // SCREEN: Playlist playback-time status.
        Win32Ui::Impl::DrawBevel(Rect(std::max(x, controls.right - 104), controls.top + 2, controls.right, controls.bottom),
                  b[5].Get(), b[3].Get(), b[4].Get(), true);
        Win32Ui::Impl::DrawText(status, Rect(std::max(x, controls.right - 101), controls.top + 2,
                              controls.right - 3, controls.bottom), b[6].Get(), tinyFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }

} // namespace rivan::ui
