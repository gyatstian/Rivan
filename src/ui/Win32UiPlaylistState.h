// Win32UiPlaylistState.h
// Playlist search, browsing, selection, drag, and inline editor state.
#pragma once

#include <d2d1.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace rivan::ui {

struct Win32UiPlaylistState {
    enum class SearchTarget : std::uint8_t { None, Playlist };

    std::wstring playlistQuery;
    bool playlistQuerySelectAll{};
    SearchTarget activeSearch{SearchTarget::None};
    std::size_t playlistScroll{};
    std::size_t playlistSearchScroll{};
    std::size_t playlistRows{};
    std::size_t playlistSearchRows{};
    D2D1_RECT_F playlistListBounds{};
    D2D1_RECT_F playlistSearchBounds{};
    D2D1_RECT_F treeListBounds{};
    std::size_t treeScroll{};

    std::set<std::size_t> trackSelection;
    std::size_t trackAnchor{static_cast<std::size_t>(-1)};
    std::uint64_t trackSelectionPlaylist{};
    std::size_t lastPlayingModelIndex{static_cast<std::size_t>(-1)};
    std::set<std::uint64_t> playlistSelection;
    std::uint64_t playlistAnchorId{};

    enum class DragKind : std::uint8_t { None, Track, Playlist };
    DragKind dragKind{DragKind::None};
    bool dragActive{};
    D2D1_POINT_2F dragStart{};
    std::size_t dragTrackIndex{static_cast<std::size_t>(-1)};
    std::uint64_t dragTrackPlaylistId{};
    std::uint64_t dragPlaylistId{};
    std::uint64_t dragPlaylistParent{};
    bool pendingTrackActivate{};
    std::uint64_t pendingTrackActivateId{};
    std::size_t dropTrackIndex{static_cast<std::size_t>(-1)};
    std::uint64_t dropBeforePlaylistId{};
    std::uint64_t dropIntoPlaylistId{};
    bool dropAtPlaylistEnd{};

    bool playlistNameEditing{};
    bool playlistNameRenaming{};
    std::uint64_t playlistRenameId{};
    std::wstring playlistNameBuffer;
    D2D1_RECT_F newPlaylistButtonBounds{};
    bool trackNameEditing{};
    std::size_t trackRenameIndex{static_cast<std::size_t>(-1)};
    std::wstring trackNameBuffer;
    std::size_t trackNameCursor{};
    bool trackNameSelectAll{};
};

} // namespace rivan::ui
