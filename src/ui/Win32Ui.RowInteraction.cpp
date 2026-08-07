// Win32Ui.RowInteraction.cpp
// Row and list scrolling for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::Scroll(float x, float y, int direction) {
    auto adjust = [direction](std::size_t& value, std::size_t itemCount, std::size_t rows) {
        const long long maximum = itemCount > rows ? static_cast<long long>(itemCount - rows) : 0;
        value = static_cast<std::size_t>(std::clamp<long long>(
            static_cast<long long>(value) + direction, 0, maximum));
    };
    if (Contains(playlistSearchBounds, x, y)) {
        if (model.youtubeBrowsing) {
            adjust(playlistSearchScroll, model.youtubeResults.size(), playlistSearchRows);
        } else {
            // Row count includes visible section headers, approximated by track+section.
            adjust(playlistSearchScroll,
                   Filtered(model.tracks, playlistQuery).size() + model.trackSections.size(),
                   playlistSearchRows);
        }
    } else if (Contains(playlistListBounds, x, y)) {
        adjust(playlistScroll, Filtered(model.tracks, L"").size(), playlistRows);
    } else if (Contains(treeListBounds, x, y)) {
        adjust(treeScroll, model.playlists.size(), 1);
    } else if (windowKind == WindowKind::SkinStudio && studioLayersTab &&
               Contains(studioLayerBounds, x, y)) {
        adjust(studioLayerScroll, studioDraft.images.size() + studioDraft.shapes.size(), studioLayerRows);
    } else if (windowKind == WindowKind::SkinStudio && !studioLayersTab &&
               Contains(studioImageListBounds, x, y)) {
        adjust(studioImageScroll, studioDraft.images.size(), studioImageRows);
    } else if (windowKind == WindowKind::Settings &&
               Contains(settingsSkinListBounds, x, y)) {
        adjust(settingsSkinScroll, model.skins.size(), settingsSkinRows);
    } else if (windowKind == WindowKind::Settings &&
               Contains(settingsDetailsBounds, x, y)) {
        // General pane uses pixel scroll; wheel direction matches list rows (~24px steps).
        const float viewportH = std::max(
            0.0F, (settingsDetailsBounds.bottom - 4.0F) - (settingsDetailsBounds.top + 50.0F));
        const float maxScroll = std::max(0.0F, settingsContentHeight - viewportH);
        settingsScrollY = std::clamp(
            settingsScrollY + static_cast<float>(direction) * 24.0F, 0.0F, maxScroll);
    }
    InvalidateRect(window, nullptr, FALSE);
}

bool Win32Ui::Impl::IsUserPlaylistId(std::uint64_t id) const noexcept {
    for (const auto& playlist : model.playlists) {
        if (playlist.id == id) return playlist.user;
    }
    return false;
}

bool Win32Ui::Impl::IsReorderablePlaylistId(std::uint64_t id) const noexcept {
    for (const auto& playlist : model.playlists) {
        if (playlist.id == id) return playlist.reorderable;
    }
    return false;
}

std::uint64_t Win32Ui::Impl::PlaylistReorderParent(std::uint64_t id) const noexcept {
    for (const auto& playlist : model.playlists) {
        if (playlist.id == id) return playlist.parentId;
    }
    return ~1ULL;
}

void Win32Ui::Impl::ResetTrackSelectionForPlaylist(std::uint64_t playlistId) {
    if (trackSelectionPlaylist == playlistId) return;
    trackSelectionPlaylist = playlistId;
    trackSelection.clear();
    trackAnchor = static_cast<std::size_t>(-1);
}

std::vector<std::size_t> Win32Ui::Impl::SelectedTrackIndicesSorted() const {
    return std::vector<std::size_t>(trackSelection.begin(), trackSelection.end());
}

void Win32Ui::Impl::ApplyTrackClickSelection(std::size_t modelIndex, bool ctrl, bool shift) {
    ResetTrackSelectionForPlaylist(model.selectedPlaylistId);
    if (modelIndex >= model.tracks.size()) return;
    if (shift && trackAnchor != static_cast<std::size_t>(-1)) {
        const std::size_t lo = std::min(trackAnchor, modelIndex);
        const std::size_t hi = std::max(trackAnchor, modelIndex);
        if (!ctrl) trackSelection.clear();
        for (std::size_t i = lo; i <= hi && i < model.tracks.size(); ++i) {
            trackSelection.insert(i);
        }
        return;
    }
    if (ctrl) {
        if (!trackSelection.insert(modelIndex).second) trackSelection.erase(modelIndex);
        trackAnchor = modelIndex;
        return;
    }
    trackSelection.clear();
    trackSelection.insert(modelIndex);
    trackAnchor = modelIndex;
}

void Win32Ui::Impl::ApplyPlaylistClickSelection(std::uint64_t id, std::size_t treeIndex,
                                                bool ctrl, bool shift) {
    if ((ctrl || shift) && IsUserPlaylistId(id)) {
        if (shift && playlistAnchorId != 0) {
            std::size_t anchorIndex = static_cast<std::size_t>(-1);
            for (std::size_t i = 0; i < model.playlists.size(); ++i) {
                if (model.playlists[i].id == playlistAnchorId) { anchorIndex = i; break; }
            }
            if (anchorIndex != static_cast<std::size_t>(-1)) {
                const std::size_t lo = std::min(anchorIndex, treeIndex);
                const std::size_t hi = std::max(anchorIndex, treeIndex);
                if (!ctrl) playlistSelection.clear();
                for (std::size_t i = lo; i <= hi && i < model.playlists.size(); ++i) {
                    if (model.playlists[i].user) playlistSelection.insert(model.playlists[i].id);
                }
                return;
            }
        }
        if (!playlistSelection.insert(id).second) playlistSelection.erase(id);
        playlistAnchorId = id;
        return;
    }
    // Plain click: clear multi-selection and open the playlist.
    playlistSelection.clear();
    playlistAnchorId = id;
    playlistQuery.clear();
    playlistQuerySelectAll = false;
    playlistSearchScroll = 0;
    if (window) KillTimer(window, kYoutubeSearchDebounceTimer);
    try { host.SelectPlaylist(id); } catch (...) {}
}

void Win32Ui::Impl::BeginTrackPress(const HitRegion& hit, float x, float y) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    ApplyTrackClickSelection(hit.index, ctrl, shift);
    // Defer play to release: a plain single click on an already-mono-selected row plays;
    // ctrl/shift never play. A press-drag on an editable playlist reorders instead.
    pendingTrackActivate = !ctrl && !shift;
    pendingTrackActivateId = hit.id;
    dragKind = DragKind::Track;
    dragActive = false;
    dragStart = {x, y};
    dragTrackIndex = hit.index;
    dragTrackPlaylistId = hit.index < model.tracks.size()
                              ? model.tracks[hit.index].sourcePlaylistId
                              : 0;
    dropTrackIndex = static_cast<std::size_t>(-1);
    SetCapture(window);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::BeginPlaylistPress(const HitRegion& hit, float x, float y) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    ApplyPlaylistClickSelection(hit.id, hit.index, ctrl, shift);
    // Directory folders and user playlists reorder by drag; others just select/open.
    if (IsReorderablePlaylistId(hit.id) && !ctrl && !shift) {
        dragKind = DragKind::Playlist;
        dragActive = false;
        dragStart = {x, y};
        dragPlaylistId = hit.id;
        dragPlaylistParent = PlaylistReorderParent(hit.id);
        dropBeforePlaylistId = 0;
        dropIntoPlaylistId = 0;
        dropAtPlaylistEnd = false;
        SetCapture(window);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::UpdateRowDrag(float x, float y) {
    if (!dragActive) {
        // Promote to an active drag only after moving past a small threshold, so a
        // shaky click is not mistaken for a reorder.
        const float dx = x - dragStart.x;
        const float dy = y - dragStart.y;
        if (dx * dx + dy * dy < 25.0F) return;
        // Parent-folder views contain rows from descendant folders. The source playlist
        // carried by the row tells us which direct list is safe to reorder.
        if (dragKind == DragKind::Track &&
            (!model.selectedPlaylistTracksReorderable || dragTrackPlaylistId == 0)) return;
        dragActive = true;
    }
    if (dragKind == DragKind::Track) {
        dropTrackIndex = ResolveTrackDrop(y);
    } else if (dragKind == DragKind::Playlist) {
        ResolvePlaylistDrop(y);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::FinishRowDrag() noexcept {
    const DragKind kind = dragKind;
    const bool wasActive = dragActive;
    dragKind = DragKind::None;
    dragActive = false;
    try {
        if (wasActive && kind == DragKind::Track) {
            FinishTrackDrag();
        } else if (wasActive && kind == DragKind::Playlist) {
            FinishPlaylistDrag();
        } else if (!wasActive && kind == DragKind::Track && pendingTrackActivate) {
            // A plain click that never became a drag: play the row.
            host.ActivateTrack(pendingTrackActivateId);
        }
    } catch (...) {}
    pendingTrackActivate = false;
    dropTrackIndex = static_cast<std::size_t>(-1);
    dragTrackPlaylistId = 0;
    dropBeforePlaylistId = 0;
    dropIntoPlaylistId = 0;
    dropAtPlaylistEnd = false;
    InvalidateRect(window, nullptr, FALSE);
}

std::size_t Win32Ui::Impl::ResolveTrackDrop(float y) const {
    // Use rendered hit rows instead of playlistListBounds: Directory folders render in
    // the current-folder pane, while user playlists render in the editor pane. Restrict
    // the target to the source playlist of the row being dragged.
    if (model.tracks.empty() || dragTrackPlaylistId == 0) return 0;
    const HitRegion* first = nullptr;
    const HitRegion* last = nullptr;
    std::size_t firstSourceIndex = 0;
    std::size_t lastSourceIndex = 0;
    for (const auto& hit : hits) {
        // Both current-folder and playlist-editor panes can contribute track hits. Restrict
        // this drag to its original pane; otherwise a same-height row in the other pane
        // can replace the correct insertion target after a redraw.
        if (hit.kind != HitKind::Track || hit.index >= model.tracks.size() ||
            model.tracks[hit.index].sourcePlaylistId != dragTrackPlaylistId ||
            dragStart.x < hit.bounds.left || dragStart.x >= hit.bounds.right) continue;
        const auto sourceIndex = SourceTrackIndex(&model.tracks[hit.index]);
        if (sourceIndex == static_cast<std::size_t>(-1)) continue;
        if (first == nullptr || hit.bounds.top < first->bounds.top) {
            first = &hit;
            firstSourceIndex = sourceIndex;
        }
        if (last == nullptr || hit.bounds.top > last->bounds.top) {
            last = &hit;
            lastSourceIndex = sourceIndex;
        }
        if (y >= hit.bounds.top && y < hit.bounds.bottom) {
            return y > (hit.bounds.top + hit.bounds.bottom) * 0.5F
                       ? sourceIndex + 1
                       : sourceIndex;
        }
    }
    if (first == nullptr) return 0;
    if (y <= first->bounds.top) return firstSourceIndex;
    return lastSourceIndex + 1;
}

void Win32Ui::Impl::ResolvePlaylistDrop(float y) {
    dropBeforePlaylistId = 0;
    dropIntoPlaylistId = 0;
    dropAtPlaylistEnd = false;
    constexpr float rowH = 20.0F;
    if (model.playlists.empty()) return;
    // A reorder is scoped to the dragged row's sibling group: only rows that share its
    // parentId are valid snap targets. dropAtPlaylistEnd means "end of that group"
    // (beforeId 0), which the manager resolves against the dragged folder's own parent.
    const std::uint64_t group = dragPlaylistParent;
    const auto isSibling = [&](std::size_t i) noexcept {
        return model.playlists[i].reorderable && model.playlists[i].parentId == group;
    };
    const float local = y - treeListBounds.top;
    const long long rel = static_cast<long long>(std::floor(local / rowH));
    const long long rowUnder = static_cast<long long>(treeScroll) + rel;
    std::size_t targetRow = 0;
    if (rowUnder < 0) {
        targetRow = 0;  // Above the list: snap before the first sibling.
    } else if (rowUnder >= static_cast<long long>(model.playlists.size())) {
        dropAtPlaylistEnd = true;
        return;
    } else {
        const auto index = static_cast<std::size_t>(rowUnder);
        const float rowTop = treeListBounds.top +
            static_cast<float>(index - treeScroll) * rowH;
        // Dropping over a row's middle moves a filesystem playlist into that folder.
        // Top and bottom quarters retain precise sibling ordering behavior.
        const bool sourceIsFolder = dragPlaylistParent != ui::kUserPlaylistGroupParent;
        const auto& hovered = model.playlists[index];
        const bool targetIsFolder = hovered.reorderable &&
                                    hovered.parentId != ui::kUserPlaylistGroupParent;
        if (sourceIsFolder && targetIsFolder && hovered.id != dragPlaylistId &&
            y >= rowTop + rowH * 0.25F && y <= rowTop + rowH * 0.75F) {
            dropIntoPlaylistId = hovered.id;
            return;
        }
        const bool lowerHalf = y > rowTop + rowH * 0.5F;
        targetRow = lowerHalf ? index + 1 : index;
    }
    if (targetRow >= model.playlists.size()) { dropAtPlaylistEnd = true; return; }
    // Snap to the next sibling at/after target; else append to the end of the group.
    for (std::size_t i = targetRow; i < model.playlists.size(); ++i) {
        if (isSibling(i)) { dropBeforePlaylistId = model.playlists[i].id; return; }
    }
    dropAtPlaylistEnd = true;
}

void Win32Ui::Impl::FinishTrackDrag() {
    if (!model.selectedPlaylistTracksReorderable ||
        dropTrackIndex == static_cast<std::size_t>(-1)) return;
    const auto selected = SelectedTrackIndicesSorted();
    if (selected.empty() || dragTrackPlaylistId == 0) return;
    // A mixed selection cannot be represented by one playlist reorder. Do not silently
    // move only part of it when rows from multiple subfolders are selected.
    for (const auto index : selected) {
        if (index >= model.tracks.size() ||
            model.tracks[index].sourcePlaylistId != dragTrackPlaylistId) return;
    }
    std::vector<std::size_t> sourceIndices;
    sourceIndices.reserve(selected.size());
    for (std::size_t index = 0; index < model.tracks.size(); ++index) {
        if (model.tracks[index].sourcePlaylistId != dragTrackPlaylistId) continue;
        if (std::binary_search(selected.begin(), selected.end(), index)) {
            sourceIndices.push_back(SourceTrackIndex(&model.tracks[index]));
        }
    }
    host.ReorderSelectedTracks(dragTrackPlaylistId, sourceIndices, dropTrackIndex);
    trackSelection.clear();
    trackAnchor = static_cast<std::size_t>(-1);
}

void Win32Ui::Impl::FinishPlaylistDrag() {
    if (dragPlaylistId == 0) return;
    if (dropIntoPlaylistId != 0) {
        host.MovePlaylistInto(dragPlaylistId, dropIntoPlaylistId);
        dragPlaylistId = 0;
        return;
    }
    if (!dropAtPlaylistEnd && dropBeforePlaylistId == 0) return;
    if (dropBeforePlaylistId == dragPlaylistId) return;
    host.ReorderUserPlaylist(dragPlaylistId, dropAtPlaylistEnd ? 0 : dropBeforePlaylistId);
    dragPlaylistId = 0;
}

void Win32Ui::Impl::RemoveSelectedTracks() {
    if (!model.selectedPlaylistIsUser) {
        MessageBoxW(window,
                    L"REM removes songs from a user playlist. Select a playlist you created "
                    L"with the + button first.",
                    L"Rivan", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto indices = SelectedTrackIndicesSorted();
    if (indices.empty()) {
        MessageBoxW(window,
                    L"Select one or more songs in the playlist editor, then press REM.",
                    L"Rivan", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (model.selectedPlaylistDeletesFiles) {
        const std::wstring count = std::to_wstring(indices.size());
        const std::wstring message =
            L"Delete " + count + L" selected song" + (indices.size() == 1 ? L"" : L"s") +
            L" from disk?\n\nREM permanently removes these source file" +
            (indices.size() == 1 ? L"" : L"s") + L". This cannot be undone.";
        if (MessageBoxW(window, message.c_str(), L"Delete music files",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
            return;
        }
    }
    try { host.RemoveTracksAt(indices); } catch (...) {}
    trackSelection.clear();
    trackAnchor = static_cast<std::size_t>(-1);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::BeginCreatePlaylist() {
    playlistNameEditing = true;
    playlistNameRenaming = false;
    playlistRenameId = 0;
    playlistNameBuffer.clear();
    activeSearch = SearchTarget::None;
    SetFocus(window);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CommitPlaylistName() {
    const std::wstring name = playlistNameBuffer;
    const bool renaming = playlistNameRenaming;
    const std::uint64_t renameId = playlistRenameId;
    playlistNameEditing = false;
    playlistNameRenaming = false;
    playlistNameBuffer.clear();
    if (name.empty()) { InvalidateRect(window, nullptr, FALSE); return; }
    try {
        if (renaming) host.RenameUserPlaylist(renameId, name);
        else host.CreateUserPlaylist(name);
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CancelPlaylistName() {
    playlistNameEditing = false;
    playlistNameRenaming = false;
    playlistNameBuffer.clear();
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CommitTrackName() {
    const std::size_t index = trackRenameIndex;
    const std::wstring name = trackNameBuffer;
    trackNameEditing = false;
    trackRenameIndex = static_cast<std::size_t>(-1);
    trackNameBuffer.clear();
    trackNameCursor = 0;
    trackNameSelectAll = false;
    if (!name.empty()) {
        try { host.RenameTrackAt(index, name); } catch (...) {}
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CancelTrackName() {
    trackNameEditing = false;
    trackRenameIndex = static_cast<std::size_t>(-1);
    trackNameBuffer.clear();
    trackNameCursor = 0;
    trackNameSelectAll = false;
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::BeginTrackRename(std::size_t modelIndex) {
    if (modelIndex >= model.tracks.size()) return;
    trackNameEditing = true;
    trackRenameIndex = modelIndex;
    const auto path = std::filesystem::path(model.tracks[modelIndex].filePath);
    trackNameBuffer = path.stem().wstring();
    trackNameCursor = trackNameBuffer.size();
    trackNameSelectAll = !trackNameBuffer.empty();
    SetFocus(window);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::PointerRightDown(float x, float y) {
    if (windowKind != WindowKind::Main || model.miniPlayer) return;
    if (HasTitlebar()) y -= kTitlebarHeight;
    SetFocus(window);
    // Menus depend on playlist permissions; repaint can lag a recent selection.
    try { host.SnapshotUiModel(model); } catch (...) {}
    mouse = {static_cast<LONG>(x), static_cast<LONG>(y)};
    const HitRegion* found = HitTestContent(x, y);
    if (found == nullptr) return;
    if (found->kind == HitKind::Track) {
        ResetTrackSelectionForPlaylist(model.selectedPlaylistId);
        // Right-clicking outside the current selection re-selects just that row.
        if (!trackSelection.contains(found->index)) {
            trackSelection.clear();
            trackSelection.insert(found->index);
            trackAnchor = found->index;
            InvalidateRect(window, nullptr, FALSE);
        }
        ShowTrackContextMenu(found->index);
    } else if (found->kind == HitKind::Playlist && IsUserPlaylistId(found->id)) {
        if (!playlistSelection.contains(found->id)) {
            playlistSelection.clear();
            playlistSelection.insert(found->id);
            playlistAnchorId = found->id;
            InvalidateRect(window, nullptr, FALSE);
        }
        ShowPlaylistContextMenu(found->id);
    }
}

void Win32Ui::Impl::SyncMouseFromCursor() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    ScreenToClient(window, &cursor);
    mouse = cursor;
}

void Win32Ui::Impl::ShowTrackContextMenu(std::size_t modelIndex) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    HMENU addMenu = CreatePopupMenu();
    HMENU moveMenu = CreatePopupMenu();
    // Add-to-playlist submenu: one command id per user playlist (base 3000), plus a
    // "New playlist..." entry (id 2). Remove = 3, Duplicate = 4.
    constexpr UINT kAddBase = 3000;
    constexpr UINT kMoveBase = 4000;
    std::vector<std::uint64_t> addTargets;
    std::vector<std::uint64_t> moveTargets;
    if (addMenu) {
        for (const auto& playlist : model.playlists) {
            if (!playlist.user) continue;
            AppendMenuW(addMenu, MF_STRING, kAddBase + addTargets.size(), playlist.name.c_str());
            addTargets.push_back(playlist.id);
        }
        if (!addTargets.empty()) AppendMenuW(addMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(addMenu, MF_STRING, 2, L"New playlist...");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(addMenu), L"Add to playlist");
    }
    if (moveMenu) {
        for (const auto& playlist : model.playlists) {
            if (!playlist.user || playlist.id == model.selectedPlaylistId) continue;
            AppendMenuW(moveMenu, MF_STRING, kMoveBase + moveTargets.size(), playlist.name.c_str());
            moveTargets.push_back(playlist.id);
        }
        if (moveTargets.empty()) {
            AppendMenuW(moveMenu, MF_STRING | MF_GRAYED, kMoveBase, L"No other playlists");
        }
        AppendMenuW(menu, MF_POPUP | (model.selectedPlaylistCanMoveTracks ? 0U : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(moveMenu), L"Move to playlist");
    }
    // The snapshot records selected playlist permissions directly. Looking it up again
    // in tree rows can be stale while selection and repaint are being synchronized.
    const bool editable = model.selectedPlaylistIsUser;
    const wchar_t* removeLabel = model.selectedPlaylistDeletesFiles ? L"Delete from disk" : L"Remove";
    AppendMenuW(menu, MF_STRING | (editable ? 0U : MF_GRAYED), 3, removeLabel);
    AppendMenuW(menu, MF_STRING | (editable ? 0U : MF_GRAYED), 4, L"Duplicate");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"Rename");
    const bool hasAudio = std::any_of(trackSelection.begin(), trackSelection.end(), [this](std::size_t index) {
        return index < model.tracks.size() && model.tracks[index].audioFile;
    });
    AppendMenuW(menu, MF_STRING | (hasAudio ? 0U : MF_GRAYED), 6, L"Change cover");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    const int command = static_cast<int>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window, nullptr));
    DestroyMenu(menu);
    SyncMouseFromCursor();
    if (command == 0) {
        InvalidateRect(window, nullptr, FALSE);
        return;
    }

    const auto indices = SelectedTrackIndicesSorted();
    try {
        if (command == 2) {
            // New playlist from selection: create empty, then add the selection to it once
            // it becomes the selected playlist. Simpler path: create then add by capturing.
            BeginCreatePlaylist();
        } else if (command == 3) {
            RemoveSelectedTracks();
        } else if (command == 4) {
            if (!indices.empty()) host.DuplicateTracksAt(indices);
        } else if (command == 5 && modelIndex < model.tracks.size()) {
            BeginTrackRename(modelIndex);
        } else if (command == 6) {
            if (!indices.empty()) {
                host.ChangeTracksCover(indices);
                trackCoverCache.clear();
                trackCoverUseCounter = 0;
                nextTrackCoverLookup = {};
            }
        } else if (command >= static_cast<int>(kMoveBase)) {
            const std::size_t which = static_cast<std::size_t>(command) - kMoveBase;
            if (which < moveTargets.size() && !indices.empty()) {
                host.MoveTracksToPlaylist(moveTargets[which], indices);
            }
        } else if (command >= static_cast<int>(kAddBase)) {
            const std::size_t which = static_cast<std::size_t>(command) - kAddBase;
            if (which < addTargets.size() && !indices.empty()) {
                host.AddTracksToPlaylist(addTargets[which], indices);
            }
        }
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::ShowPlaylistContextMenu(std::uint64_t playlistId) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"Rename");
    AppendMenuW(menu, MF_STRING, 2, L"Delete");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    const int command = static_cast<int>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window, nullptr));
    DestroyMenu(menu);
    SyncMouseFromCursor();
    if (command == 1) {
        // Inline rename of the clicked playlist.
        playlistNameEditing = true;
        playlistNameRenaming = true;
        playlistRenameId = playlistId;
        for (const auto& playlist : model.playlists) {
            if (playlist.id == playlistId) { playlistNameBuffer = playlist.name; break; }
        }
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
    } else if (command == 2) {
        // Delete the whole multi-selection when the clicked row is part of it.
        std::vector<std::uint64_t> ids;
        if (playlistSelection.contains(playlistId)) {
            ids.assign(playlistSelection.begin(), playlistSelection.end());
        } else {
            ids.push_back(playlistId);
        }
        std::size_t folders = 0;
        std::size_t tracks = 0;
        for (std::size_t i = 0; i < model.playlists.size(); ++i) {
            const auto& playlist = model.playlists[i];
            if (std::find(ids.begin(), ids.end(), playlist.id) == ids.end()) continue;
            bool coveredBySelectedParent = false;
            for (std::size_t parent = i; parent-- > 0;) {
                if (model.playlists[parent].depth >= playlist.depth) continue;
                coveredBySelectedParent =
                    std::find(ids.begin(), ids.end(), model.playlists[parent].id) != ids.end();
                break;
            }
            if (coveredBySelectedParent) continue;
            ++folders;
            tracks += playlist.trackCount;
        }
        const std::wstring message =
            L"Delete " + std::to_wstring(folders) + L" folder" + (folders == 1 ? L"" : L"s") +
            L" and " + std::to_wstring(tracks) + L" music file" +
            (tracks == 1 ? L"" : L"s") +
            L" from disk?\n\nThis permanently deletes every selected folder and its contents. "
            L"This cannot be undone.";
        if (MessageBoxW(window, message.c_str(), L"Delete music folders",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        playlistSelection.clear();
        try { host.DeleteUserPlaylists(ids); } catch (...) {}
        InvalidateRect(window, nullptr, FALSE);
    } else {
        InvalidateRect(window, nullptr, FALSE);
    }
}

} // namespace rivan::ui
