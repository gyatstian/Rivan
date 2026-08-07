// Win32Ui.SkinStudio.Input.cpp
// Clipboard input methods that remained with Skin Studio during its split.
#include "Win32UiImpl.h"

namespace rivan::ui {

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

} // namespace rivan::ui
