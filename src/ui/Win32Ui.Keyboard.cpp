// Win32Ui.Keyboard.cpp
// Keyboard, character input, and scrolling methods for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::ActivateFirstSearchResult() {
    if (model.youtubeBrowsing) {
        try {
            if (!playlistQuery.empty()) {
                if (window) KillTimer(window, kYoutubeSearchDebounceTimer);
                host.SubmitYoutubeQuery(playlistQuery);
            } else if (!model.youtubeResults.empty()) {
                host.ActivateYoutubeResult(model.youtubeResults.front().id);
            }
        } catch (...) {}
        return;
    }
    const std::wstring lowerQuery = Lowercase(playlistQuery);
    for (const auto& track : model.tracks) {
        if (MatchesLowered(track, lowerQuery)) {
            try { host.ActivateTrack(track.id); } catch (...) {}
            return;
        }
    }
}

void Win32Ui::Impl::Character(wchar_t character) {
    if (trackNameEditing) {
        trackNameCursor = std::min(trackNameCursor, trackNameBuffer.size());
        if (character == L'\r') {
            CommitTrackName();
            return;
        }
        // Ctrl shortcuts emit WM_CHAR control codes after WM_KEYDOWN. Keep the selected
        // state established by Ctrl+A so the next typed or pasted text replaces the name.
        if (character != L'\b' && (character < L' ' || character == 0x7FU)) return;
        if (character == L'\b') {
            if (trackNameSelectAll) {
                trackNameBuffer.clear();
                trackNameCursor = 0;
            } else if (trackNameCursor > 0) {
                trackNameBuffer.erase(trackNameCursor - 1, 1);
                --trackNameCursor;
            }
        } else if (character >= L' ' && character != 0x7FU && trackNameBuffer.size() < 180) {
            if (trackNameSelectAll) {
                trackNameBuffer.clear();
                trackNameCursor = 0;
            }
            trackNameBuffer.insert(trackNameCursor, 1, character);
            ++trackNameCursor;
        }
        trackNameSelectAll = false;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (playlistNameEditing) {
        if (character == L'\r') {
            CommitPlaylistName();
            return;
        }
        if (character == L'\b') {
            if (!playlistNameBuffer.empty()) playlistNameBuffer.pop_back();
        } else if (character >= L' ' && character != 0x7FU && playlistNameBuffer.size() < 96) {
            playlistNameBuffer.push_back(character);
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (studioNameEditing || managerNameEditing) {
        auto& name = studioNameEditing ? studioName : managerSkinName;
        if (character == L'\r') {
            HandleStudioAction(studioNameEditing ? 96 : 900 + managerSkinIndex);
            return;
        }
        if (character == L'\b') {
            if (!name.empty()) name.pop_back();
        } else if (character >= L' ' && character != 0x7FU && name.size() < 128) {
            name.push_back(character);
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (studioHexEditing) {
        if (character == L'\r') {
            ApplyStudioHex();
            return;
        }
        if (character == L'\b') {
            if (studioHexSelectAll) studioHex.clear();
            else if (!studioHex.empty()) studioHex.pop_back();
        } else if ((character == L'#' && studioHex.empty()) || std::iswxdigit(character)) {
            if (studioHexSelectAll) studioHex.clear();
            if (studioHex.size() < 9) {
                studioHex.push_back(static_cast<wchar_t>(std::towupper(character)));
            }
        }
        studioHexSelectAll = false;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (activeSearch == SearchTarget::None) return;
    // Ctrl/Alt produce WM_CHAR control codes (e.g. Ctrl+A = 1). Ignore them so
    // KeyDown's select-all / paste handling is not immediately cleared.
    if (character != L'\b' && (character < L' ' || character == 0x7FU)) return;
    if (character == L'\b') {
        if (playlistQuerySelectAll) playlistQuery.clear();
        else if (!playlistQuery.empty()) playlistQuery.pop_back();
    } else {
        if (playlistQuerySelectAll) playlistQuery.clear();
        if (playlistQuery.size() < 120) playlistQuery.push_back(character);
    }
    playlistQuerySelectAll = false;
    playlistSearchScroll = 0;
    ArmYoutubeSearchDebounce();
    InvalidateRect(window, nullptr, FALSE);
}

bool Win32Ui::Impl::KeyDown(WPARAM key) {
    if (windowKind == WindowKind::Settings && youtubeGrabberHotkeyCapture) {
        if (key == VK_ESCAPE) {
            youtubeGrabberHotkeyCapture = false;
            youtubeGrabberHotkeyCaptureFailed = false;
        } else if (key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU &&
                   key != VK_LWIN && key != VK_RWIN) {
            std::uint32_t modifiers{};
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) modifiers |= MOD_CONTROL;
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= MOD_SHIFT;
            if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= MOD_ALT;
            if ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
                (GetKeyState(VK_RWIN) & 0x8000) != 0) {
                modifiers |= MOD_WIN;
            }
            bool registered = false;
            try {
                registered = host.SetYoutubeGrabberHotkey(
                    modifiers, static_cast<std::uint32_t>(key));
            } catch (...) {}
            youtubeGrabberHotkeyCaptureFailed = !registered;
            if (registered) youtubeGrabberHotkeyCapture = false;
        }
        InvalidateRect(window, nullptr, FALSE);
        return true;
    }
    if (previewFullscreen && windowKind == WindowKind::Main) {
        if (key == VK_ESCAPE) {
            ExitPreviewFullscreen();
            return true;
        }
        // Keep media keys working under fullscreen; swallow the rest.
        Command media{};
        bool mediaKey = true;
        switch (key) {
        case VK_SPACE:
        case L'K':
        case VK_MEDIA_PLAY_PAUSE: media = Command::PlayPause; break;
        case VK_LEFT: media = Command::SeekBackward; break;
        case VK_RIGHT: media = Command::SeekForward; break;
        case VK_MEDIA_PREV_TRACK: media = Command::Previous; break;
        case VK_MEDIA_NEXT_TRACK: media = Command::Next; break;
        case VK_MEDIA_STOP: media = Command::Stop; break;
        case VK_UP:
        case VK_VOLUME_UP: media = Command::VolumeUp; break;
        case VK_DOWN:
        case VK_VOLUME_DOWN: media = Command::VolumeDown; break;
        default: mediaKey = false; break;
        }
        if (mediaKey) InvokeSafely(media);
        return true;
    }
    if (trackNameEditing) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (control && (key == L'A' || key == L'a')) {
            trackNameSelectAll = !trackNameBuffer.empty();
            trackNameCursor = trackNameBuffer.size();
            InvalidateRect(window, nullptr, FALSE);
        } else if (control && (key == L'C' || key == L'c')) {
            CopyTrackName();
        } else if (control && (key == L'V' || key == L'v')) {
            PasteTrackName();
        } else if (key == VK_LEFT) {
            if (trackNameSelectAll) trackNameCursor = 0;
            else if (trackNameCursor > 0) --trackNameCursor;
            trackNameSelectAll = false;
            InvalidateRect(window, nullptr, FALSE);
        } else if (key == VK_RIGHT) {
            if (trackNameSelectAll) trackNameCursor = trackNameBuffer.size();
            else if (trackNameCursor < trackNameBuffer.size()) ++trackNameCursor;
            trackNameSelectAll = false;
            InvalidateRect(window, nullptr, FALSE);
        } else if (key == VK_HOME) {
            trackNameCursor = 0;
            trackNameSelectAll = false;
            InvalidateRect(window, nullptr, FALSE);
        } else if (key == VK_END) {
            trackNameCursor = trackNameBuffer.size();
            trackNameSelectAll = false;
            InvalidateRect(window, nullptr, FALSE);
        } else if (key == VK_DELETE) {
            trackNameCursor = std::min(trackNameCursor, trackNameBuffer.size());
            if (trackNameSelectAll) {
                trackNameBuffer.clear();
                trackNameCursor = 0;
            } else if (trackNameCursor < trackNameBuffer.size()) {
                trackNameBuffer.erase(trackNameCursor, 1);
            }
            trackNameSelectAll = false;
            InvalidateRect(window, nullptr, FALSE);
        } else if (key == VK_RETURN) CommitTrackName();
        else if (key == VK_ESCAPE) CancelTrackName();
        return true;
    }
    if (playlistNameEditing) {
        if (key == VK_RETURN) CommitPlaylistName();
        else if (key == VK_ESCAPE) CancelPlaylistName();
        return true;
    }
    if (pickingScreenColor) {
        if (key == VK_ESCAPE) CancelScreenEyedropper();
        return true;
    }
    if (studioNameEditing || managerNameEditing) {
        if (key == VK_RETURN) HandleStudioAction(studioNameEditing ? 96 : 900 + managerSkinIndex);
        else if (key == VK_ESCAPE) {
            studioNameEditing = false;
            managerNameEditing = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return true;
    }
    if (studioHexEditing) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (control && key == L'A') {
            studioHexSelectAll = true;
            InvalidateRect(window, nullptr, FALSE);
        } else if (control && key == L'C') CopyStudioHex();
        else if (control && key == L'V') PasteStudioHex();
        else if (key == VK_RETURN) ApplyStudioHex();
        else if (key == VK_ESCAPE) {
            studioHexEditing = false;
            studioHex.clear();
            studioHexSelectAll = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return true;
    }
    if (activeSearch != SearchTarget::None) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (key == VK_ESCAPE) {
            activeSearch = SearchTarget::None;
            playlistQuerySelectAll = false;
        } else if (key == VK_RETURN) ActivateFirstSearchResult();
        else if (control && (key == L'A' || key == L'a')) {
            if (!playlistQuery.empty()) {
                playlistQuerySelectAll = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            return true;
        } else if (control && (key == L'V' || key == L'v')) {
            PastePlaylistQuery();
            return true;
        }
        InvalidateRect(window, nullptr, FALSE);
        return true;
    }
    if (windowKind == WindowKind::Main) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (control && shift) {
            switch (key) {
            case L'S': HandleLyricsAction(1); return true;
            case L'P': HandleLyricsAction(2); return true;
            case L'L': HandleLyricsAction(3); return true;
            case L'E': HandleLyricsAction(4); return true;
            case L'R': HandleLyricsAction(5); return true;
            case L'C': HandleLyricsAction(6); return true;
            default: break;
            }
        }
    }
    if (windowKind == WindowKind::Main && !model.miniPlayer && key == VK_F2 && !trackSelection.empty()) {
        const auto chosen = trackSelection.contains(trackAnchor) ? trackAnchor : *trackSelection.begin();
        BeginTrackRename(chosen);
        return true;
    }
    if (windowKind == WindowKind::Main && !model.miniPlayer && key == VK_DELETE && !trackSelection.empty()) {
        RemoveSelectedTracks();
        return true;
    }
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    Command command{};
    bool handled = true;
    switch (key) {
    case VK_SPACE: command = Command::PlayPause; break;
    case VK_LEFT: command = control ? Command::Previous : Command::SeekBackward; break;
    case VK_RIGHT: command = control ? Command::Next : Command::SeekForward; break;
    case VK_UP: command = Command::VolumeUp; break;
    case VK_DOWN: command = Command::VolumeDown; break;
    case VK_ESCAPE:
        if (model.settingsVisible) command = Command::ToggleSettings;
        else handled = false;
        break;
    case L'S': command = control ? Command::ToggleSettings : Command::ToggleShuffle; break;
    case L'R': command = Command::CycleRepeat; break;
    case L'M': command = Command::ToggleMiniPlayer; break;
    case L'K': command = Command::PlayPause; break;
    case VK_MEDIA_PLAY_PAUSE: command = Command::PlayPause; break;
    case VK_MEDIA_STOP: command = Command::Stop; break;
    case VK_MEDIA_PREV_TRACK: command = Command::Previous; break;
    case VK_MEDIA_NEXT_TRACK: command = Command::Next; break;
    case VK_VOLUME_UP: command = Command::VolumeUp; break;
    case VK_VOLUME_DOWN: command = Command::VolumeDown; break;
    default: handled = false; break;
    }
    if (handled) InvokeSafely(command);
    return handled;
}

} // namespace rivan::ui
