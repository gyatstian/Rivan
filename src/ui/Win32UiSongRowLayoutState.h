// Win32UiSongRowLayoutState.h
// Transient state for the Preferences song-row layout editor.
#pragma once

#include "SongRowLayoutGeometry.h"

#include <d2d1.h>
#include <array>
#include <optional>

namespace rivan::ui {

struct Win32UiSongRowLayoutState {
    bool songRowEditorVisible{};
    SongRowLayout songRowDraft{SongRowLayout::Defaults()};
    std::optional<SongRowField> songRowSelectedField;
    D2D1_RECT_F songRowPreviewBounds{};
    std::array<D2D1_RECT_F, kSongRowFieldCount> songRowFieldBounds{};
    bool songRowDragging{};
    bool songRowDragMoved{};
    bool songRowResizing{};
    D2D1_POINT_2F songRowDragStart{};
    D2D1_POINT_2F songRowDragOffset{};
    std::optional<SongRowSnap> songRowSnap;
};

} // namespace rivan::ui
