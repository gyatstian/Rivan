// Win32Ui.Pointer.cpp
// Pointer, decor, slider, and non-client hit-test methods for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::SetSlider(const HitRegion& hit, float x) {
    const double normalized = Width(hit.bounds) > 0.0F
        ? std::clamp(static_cast<double>((x - hit.bounds.left) / Width(hit.bounds)), 0.0, 1.0)
        : 0.0;
    try {
        if (hit.kind == HitKind::Seek) host.Seek(normalized);
        else host.SetVolume(static_cast<float>(normalized));
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

// Keeps the focused element draggable through overlapping decor, then falls back to
// the topmost element under the pointer. Returns false if nothing was hit.
[[nodiscard]] bool Win32Ui::Impl::BeginDecorDrag(float x, float y) {
    if (lastCanvas.width <= 0.0F || lastCanvas.height <= 0.0F) return false;
    if (std::any_of(decorControlBounds.begin(), decorControlBounds.end(), [x, y](const auto& bounds) {
            return Contains(bounds, x, y);
        })) return false;
    if (windowKind == WindowKind::Main) studioDraft = model.activeSkin;
    const float nx = x / lastCanvas.width;
    const float ny = y / lastCanvas.height;
    if (!studioDraft.images.empty() && studioImageFocused) {
        studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
        const auto& selected = studioDraft.images[studioImageIndex];
        const float right = (selected.x + selected.width) * lastCanvas.width;
        const float bottom = (selected.y + selected.height) * lastCanvas.height;
        const float centerX = (selected.x + selected.width * 0.5F) * lastCanvas.width;
        const float top = selected.y * lastCanvas.height;
        if (std::hypot(x - centerX, y - (top - 22.0F)) <= 10.0F) {
            draggingDecor = -(static_cast<int>(studioImageIndex) + 1);
            decorDragMode = DecorDragMode::Rotate;
            const float centerY = (selected.y + selected.height * 0.5F) * lastCanvas.height;
            dragStartAngle = std::atan2(y - centerY, x - centerX);
            dragStartRotation = selected.rotation;
            SetCapture(window);
            return true;
        }
        if (std::abs(x - right) <= 10.0F && std::abs(y - bottom) <= 10.0F) {
            draggingDecor = -(static_cast<int>(studioImageIndex) + 1);
            decorDragMode = DecorDragMode::Resize;
            SetCapture(window);
            return true;
        }
        if (nx >= selected.x && nx <= selected.x + selected.width &&
            ny >= selected.y && ny <= selected.y + selected.height) {
            draggingDecor = -(static_cast<int>(studioImageIndex) + 1);
            decorDragMode = DecorDragMode::Move;
            dragOffset = {nx - selected.x, ny - selected.y};
            SetCapture(window);
            return true;
        }
    }
    if (!studioDraft.shapes.empty() && studioShapeFocused) {
        studioShapeIndex = std::min(studioShapeIndex, studioDraft.shapes.size() - 1);
        const auto& selected = studioDraft.shapes[studioShapeIndex];
        const float right = (selected.x + selected.width) * lastCanvas.width;
        const float bottom = (selected.y + selected.height) * lastCanvas.height;
        const float centerX = (selected.x + selected.width * 0.5F) * lastCanvas.width;
        const float top = selected.y * lastCanvas.height;
        if (std::hypot(x - centerX, y - (top - 22.0F)) <= 10.0F) {
            draggingDecor = static_cast<int>(studioShapeIndex) + 1;
            decorDragMode = DecorDragMode::Rotate;
            const float centerY = (selected.y + selected.height * 0.5F) * lastCanvas.height;
            dragStartAngle = std::atan2(y - centerY, x - centerX);
            dragStartRotation = selected.rotation;
            SetCapture(window);
            return true;
        }
        if (std::abs(x - right) <= 10.0F && std::abs(y - bottom) <= 10.0F) {
            draggingDecor = static_cast<int>(studioShapeIndex) + 1;
            decorDragMode = DecorDragMode::Resize;
            SetCapture(window);
            return true;
        }
        if (nx >= selected.x && nx <= selected.x + selected.width &&
            ny >= selected.y && ny <= selected.y + selected.height) {
            draggingDecor = static_cast<int>(studioShapeIndex) + 1;
            decorDragMode = DecorDragMode::Move;
            dragOffset = {nx - selected.x, ny - selected.y};
            SetCapture(window);
            return true;
        }
    }
    auto order = DecorOrder(studioDraft);
    for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator) {
        const auto ref = *iterator;
        const float left = ref.image ? studioDraft.images[ref.index].x : studioDraft.shapes[ref.index].x;
        const float top = ref.image ? studioDraft.images[ref.index].y : studioDraft.shapes[ref.index].y;
        const float width = ref.image ? studioDraft.images[ref.index].width : studioDraft.shapes[ref.index].width;
        const float height = ref.image ? studioDraft.images[ref.index].height : studioDraft.shapes[ref.index].height;
        if (nx >= left && nx <= left + width && ny >= top && ny <= top + height) {
            draggingDecor = ref.image ? -(static_cast<int>(ref.index) + 1)
                                      : static_cast<int>(ref.index) + 1;
            studioImageFocused = ref.image;
            studioShapeFocused = !ref.image;
            if (ref.image) studioImageIndex = ref.index;
            else studioShapeIndex = ref.index;
            try { host.FocusSkinElement(draggingDecor); } catch (...) {}
            decorDragMode = DecorDragMode::Move;
            dragOffset = {nx - left, ny - top};
            SetCapture(window);
            return true;
        }
    }
    return false;
}

void Win32Ui::Impl::MoveDecor(float x, float y) {
    if (lastCanvas.width <= 0.0F || lastCanvas.height <= 0.0F) return;
    const float nx = std::clamp(x / lastCanvas.width - dragOffset.x, -0.5F, 1.5F);
    const float ny = std::clamp(y / lastCanvas.height - dragOffset.y, -0.5F, 1.5F);
    if (draggingDecor > 0) {
        auto& s = studioDraft.shapes[static_cast<std::size_t>(draggingDecor - 1)];
        if (decorDragMode == DecorDragMode::Resize) {
            s.width = std::clamp(x / lastCanvas.width - s.x, 0.02F, 2.0F);
            s.height = std::clamp(y / lastCanvas.height - s.y, 0.02F, 2.0F);
        } else if (decorDragMode == DecorDragMode::Rotate) {
            const float centerX = (s.x + s.width * 0.5F) * lastCanvas.width;
            const float centerY = (s.y + s.height * 0.5F) * lastCanvas.height;
            constexpr float radiansToDegrees = 57.2957795F;
            s.rotation = std::clamp(dragStartRotation +
                (std::atan2(y - centerY, x - centerX) - dragStartAngle) * radiansToDegrees,
                -360.0F, 360.0F);
        } else {
            s.x = nx;
            s.y = ny;
        }
    } else {
        auto& img = studioDraft.images[static_cast<std::size_t>(-draggingDecor - 1)];
        if (decorDragMode == DecorDragMode::Resize) {
            img.width = std::clamp(x / lastCanvas.width - img.x, 0.02F, 2.0F);
            img.height = std::clamp(y / lastCanvas.height - img.y, 0.02F, 2.0F);
        } else if (decorDragMode == DecorDragMode::Rotate) {
            const float centerX = (img.x + img.width * 0.5F) * lastCanvas.width;
            const float centerY = (img.y + img.height * 0.5F) * lastCanvas.height;
            constexpr float radiansToDegrees = 57.2957795F;
            img.rotation = std::clamp(dragStartRotation +
                (std::atan2(y - centerY, x - centerX) - dragStartAngle) * radiansToDegrees,
                -360.0F, 360.0F);
        } else {
            img.x = nx;
            img.y = ny;
        }
    }
    QueuePreview();
}

void Win32Ui::Impl::UpdateStudioColor(float x, float y, bool hueOnly) {
    skin::Color* color = ActiveStudioColor();
    if (!color) return;
    float hue{}, saturation{}, value{};
    ColorToHsv(*color, hue, saturation, value);
    if (hueOnly) {
        hue = std::clamp((x - studioHueBounds.left) / Width(studioHueBounds), 0.0F, 1.0F);
    } else {
        saturation = std::clamp((x - studioColorPickerBounds.left) /
                                Width(studioColorPickerBounds), 0.0F, 1.0F);
        value = 1.0F - std::clamp((y - studioColorPickerBounds.top) /
                                 Height(studioColorPickerBounds), 0.0F, 1.0F);
    }
    std::uint8_t alpha = color->alpha;
    if (studioColorTarget == StudioColorTarget::Shape) {
        alpha = 255;
    } else if (studioColorTarget == StudioColorTarget::ImageTint && alpha == 0) {
        alpha = 160;
    }
    *color = HsvColor(hue, saturation, value, alpha);
    studioHex = ToHexW(*color);
    QueuePreview();
}

void Win32Ui::Impl::UpdateLayerDrag(float y) {
    auto order = DecorOrder(studioDraft);
    std::reverse(order.begin(), order.end());
    if (draggingLayer == 0 || order.empty()) return;
    const bool image = draggingLayer < 0;
    const std::size_t index = static_cast<std::size_t>(image ? -draggingLayer - 1
                                                             : draggingLayer - 1);
    const auto current = std::find_if(order.begin(), order.end(), [image, index](const DecorRef& ref) {
        return ref.image == image && ref.index == index;
    });
    if (current == order.end()) return;
    const std::size_t targetPosition = std::min(order.size() - 1,
        studioLayerScroll + static_cast<std::size_t>(std::max(0.0F,
            std::floor((y - studioLayerBounds.top) / 26.0F))));
    const std::size_t currentPosition = static_cast<std::size_t>(current - order.begin());
    if (targetPosition == currentPosition) return;
    const DecorRef dragged = *current;
    order.erase(current);
    order.insert(order.begin() + targetPosition, dragged);
    for (std::size_t position = 0; position < order.size(); ++position) {
        const auto& ref = order[position];
        auto& priority = ref.image ? studioDraft.images[ref.index].priority
                                   : studioDraft.shapes[ref.index].priority;
        priority = static_cast<std::uint8_t>(position + 1);
    }
    studioLayerDropPosition = targetPosition;
    QueuePreview();
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::PointerDown(float x, float y) {
    SetFocus(window);
    titlebarMouse = {static_cast<LONG>(x), static_cast<LONG>(y)};
    if (HasTitlebar() && y < kTitlebarHeight) {
        if (const HitRegion* titlebarHit = HitTest(x, y); titlebarHit &&
            titlebarHit->kind == HitKind::WindowControl) {
            try {
                if (titlebarHit->id == 1) ShowWindow(window, SW_MINIMIZE);
                else if (titlebarHit->id == 2) host.Invoke(Command::ToggleMiniPlayer);
                else if (titlebarHit->id == 3) PostMessageW(window, WM_CLOSE, 0, 0);
                else if (titlebarHit->id == 4) host.Invoke(Command::ToggleSettings);
            } catch (...) {}
        }
        return;
    }
    if (HasTitlebar()) y -= kTitlebarHeight;
    if (previewFullscreen && windowKind == WindowKind::Main) {
        if (Contains(previewFullscreenCloseBounds, x, y)) {
            ExitPreviewFullscreen();
            return;
        }
        // Single click on overlay body ignored (double-click exits via WM_LBUTTONDBLCLK).
        return;
    }
    if (pickingScreenColor) {
        // Commit the sample under the cursor (works for clicks outside the studio window).
        SampleScreenColorAtCursor();
        pickingScreenColor = false;
        eyedropperSkipUp = false;
        if (previewPending) PushPreview();
        if (GetCapture() == window) ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (windowKind == WindowKind::Main && model.skinStudioVisible) {
        for (auto iterator = colorFocusRegions.rbegin(); iterator != colorFocusRegions.rend(); ++iterator) {
            if (Contains(iterator->bounds, x, y)) {
                try { host.FocusSkinColor(iterator->index); } catch (...) {}
                break;
            }
        }
    }
    if (windowKind == WindowKind::SkinStudio && studioColorPickerVisible &&
        (studioSection == StudioSection::Colors ||
         studioSection == StudioSection::Elements)) {
        if (Contains(studioColorPickerBounds, x, y)) {
            draggingStudioColor = true;
            SetCapture(window);
            UpdateStudioColor(x, y, false);
            return;
        }
        if (Contains(studioHueBounds, x, y)) {
            draggingStudioHue = true;
            SetCapture(window);
            UpdateStudioColor(x, y, true);
            return;
        }
    }
    // Collapse handles are painted over module chrome and must win over the resize
    // border at the edge where they live. A click toggles; a larger motion promotes
    // the same press into a module drag (see PointerMove).
    if (windowKind == WindowKind::Main) {
        if (const HitRegion* collapseHit = HitTestContent(x, y);
            collapseHit && collapseHit->kind == HitKind::ModuleCollapseToggle) {
            collapsedArrowPress = static_cast<ModuleId>(collapseHit->id);
            collapsedArrowPressStart = {x, y};
            collapsedArrowPressBounds = collapseHit->bounds;
            collapsedArrowDragStarted = false;
            SetCapture(window);
            return;
        }
    }
    // Studio edits decor on the main canvas. Main-player buttons and track rows must
    // not steal clicks from an image/shape while the pointer is outside the studio.
    if (windowKind == WindowKind::Main && model.skinStudioVisible && BeginDecorDrag(x, y)) return;
    if (windowKind == WindowKind::Main && BeginModuleResize(x, y)) return;
    // Keep hover in sync with the press even if no WM_MOUSEMOVE arrived first
    // (common after a context menu swallows the next move).
    mouse = {static_cast<LONG>(x), static_cast<LONG>(y)};
    const HitRegion* found = HitTestContent(x, y);
    if (!found) {
        activeSearch = SearchTarget::None;
        // Empty client click clears row multi-select so a right-click highlight does not
        // stick after the user dismisses into blank space.
        if (windowKind == WindowKind::Main && !model.miniPlayer) {
            if (!trackSelection.empty() || trackAnchor != static_cast<std::size_t>(-1)) {
                trackSelection.clear();
                trackAnchor = static_cast<std::size_t>(-1);
            }
            if (!playlistSelection.empty()) {
                playlistSelection.clear();
                playlistAnchorId = 0;
            }
        }
        // When the studio is open, an empty click on the canvas grabs the topmost
        // decor element (image or shape) under the pointer for drag-move.
        if (windowKind == WindowKind::Main && model.skinStudioVisible && BeginDecorDrag(x, y)) return;
        if (windowKind == WindowKind::Main && model.skinStudioVisible) {
            studioImageFocused = false;
            studioShapeFocused = false;
            try { host.FocusSkinElement(0); } catch (...) {}
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    const HitRegion hit = *found;
    if (windowKind == WindowKind::SkinStudio && studioLayersTab &&
        hit.kind == HitKind::Studio && hit.id >= kSelectLayerBase &&
        hit.id < kSelectLayerBase + 100) {
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        const std::size_t position = static_cast<std::size_t>(hit.id - kSelectLayerBase);
        if (position < order.size()) {
            const auto& ref = order[position];
            draggingLayer = ref.image ? -(static_cast<int>(ref.index) + 1)
                                      : static_cast<int>(ref.index) + 1;
            studioLayerDropPosition = position;
            HandleStudioAction(hit.id);
            SetCapture(window);
        }
        return;
    }
    if (hit.kind != HitKind::PlaylistSearch) {
        activeSearch = SearchTarget::None;
        playlistQuerySelectAll = false;
    }
    try {
        switch (hit.kind) {
        case HitKind::Command: host.Invoke(hit.command); break;
        case HitKind::TimeToggle: showRemaining = !showRemaining; break;
        case HitKind::Playlist:
            // Selection + drag are resolved on release so a plain click still opens the
            // playlist, ctrl/shift build a multi-selection, and a press-drag reorders.
            BeginPlaylistPress(hit, x, y);
            return;
        case HitKind::PlaylistToggle: host.TogglePlaylistExpanded(hit.id); break;
        case HitKind::Refresh: host.RefreshLibrary(); break;
        case HitKind::SettingsAction: HandleSettingsAction(hit.id); return;
        case HitKind::SongRowField:
            if (windowKind == WindowKind::Settings && hit.id < kSongRowFieldCount) {
                BeginSongRowFieldDrag(static_cast<SongRowField>(hit.id), x, y);
                return;
            }
            break;
        case HitKind::LyricsAction: HandleLyricsAction(hit.id); return;
        case HitKind::LyricsVerse: HandleLyricsVerse(static_cast<std::size_t>(hit.id)); return;
        case HitKind::Track:
            // Selection now; activation (play) deferred to release when no drag happened.
            BeginTrackPress(hit, x, y);
            return;
        case HitKind::EditorAdd:
            try { host.AddFilesToSelectedPlaylist(); } catch (...) {}
            break;
        case HitKind::EditorRemove:
            RemoveSelectedTracks();
            break;
        case HitKind::NewPlaylist:
            BeginCreatePlaylist();
            break;
        case HitKind::YoutubeResult: host.ActivateYoutubeResult(hit.id); break;
        case HitKind::YoutubeChooserAction: HandleYoutubeChooserAction(hit.id); return;
        case HitKind::FilePreviewFullscreen:
            EnterPreviewFullscreen();
            break;
        case HitKind::FilePreviewExitFullscreen:
            ExitPreviewFullscreen();
            break;
        case HitKind::Setting:
            if (hit.category != model.settingsCategory) {
                settingsScrollY = 0.0F;
                settingsSkinScroll = 0;
            }
            host.SelectSettingsCategory(hit.category);
            break;
        case HitKind::PlaylistSearch: activeSearch = SearchTarget::Playlist; break;
        case HitKind::WindowControl:
            if (hit.id == 1) ShowWindow(window, SW_MINIMIZE);
            else if (hit.id == 2) host.Invoke(Command::ToggleMiniPlayer);
            else if (hit.id == 3) PostMessageW(window, WM_CLOSE, 0, 0);
            else if (hit.id == 4) host.Invoke(Command::ToggleSettings);
            break;
        case HitKind::ModuleTitle:
            if (windowKind == WindowKind::Main) {
                BeginModuleDrag(static_cast<ModuleId>(hit.id), x, y);
                return;
            }
            break;
        case HitKind::ModuleCollapseToggle:
            if (windowKind == WindowKind::Main) {
                collapsedArrowPress = static_cast<ModuleId>(hit.id);
                collapsedArrowPressStart = {x, y};
                collapsedArrowPressBounds = hit.bounds;
                collapsedArrowDragStarted = false;
                SetCapture(window);
                return;
            }
            break;
        case HitKind::ModuleTab:
            if (windowKind == WindowKind::Main) {
                auto layout = model.moduleLayout;
                const std::size_t tab = static_cast<std::size_t>(hit.id & 0xffffffffULL);
                if (tab < layout.TabCount()) {
                    const auto id = static_cast<ModuleId>((hit.id >> 32U) & 0xffffffffULL);
                    if (!layout.IsTabbed(id) || layout.TabIndex(id) != tab) break;
                    const bool wasActive = layout.GroupActiveMember(id) == id;
                    std::size_t groupIndex = 0;
                    const auto groupCount = layout.GroupTabCount(id);
                    while (groupIndex < groupCount &&
                           layout.GroupMember(id, groupIndex) != id) {
                        ++groupIndex;
                    }
                    if (groupIndex == groupCount) break;
                    layout.SetGroupActiveTab(id, groupIndex);
                    try { host.SetModuleLayout(layout); } catch (...) {}
                    // A tab click still behaves like a normal selection when released
                    // without movement. If the pointer moves, the same gesture can
                    // pull an inactive tab out of the group and place it elsewhere.
                    // The focused tab is the group's title bar: dragging it moves the
                    // merged module instead of accidentally tearing the tab out.
                    BeginModuleDrag(id, x, y, &layout, !wasActive);
                    return;
                }
            }
            break;
        case HitKind::Studio:
            HandleStudioAction(hit.id);
            return;
        case HitKind::Seek:
            draggingSeek = true;
            SetCapture(window);
            SetSlider(hit, x);
            return;
        case HitKind::Volume:
            draggingVolume = true;
            SetCapture(window);
            SetSlider(hit, x);
            return;
        }
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::PointerMove(float x, float y) {
    titlebarMouse = {static_cast<LONG>(x), static_cast<LONG>(y)};
    if (HasTitlebar()) y -= kTitlebarHeight;
    mouse = {static_cast<LONG>(x), static_cast<LONG>(y)};
    if (collapsedArrowPress) {
        const float dx = x - collapsedArrowPressStart.x;
        const float dy = y - collapsedArrowPressStart.y;
        if (!collapsedArrowDragStarted && dx * dx + dy * dy >= kCollapsedHandleDragThreshold * kCollapsedHandleDragThreshold) {
            collapsedArrowDragStarted = true;
            BeginModuleDrag(*collapsedArrowPress, collapsedArrowPressStart.x,
                            collapsedArrowPressStart.y);
            if (moduleGesture == ModuleGesture::Move) {
                moduleDragFromCollapsedArrow = true;
                moduleCollapsedArrowOrigin = collapsedArrowPressBounds;
                moduleDragActive = true;
                UpdateModuleDrag(x, y);
            }
        } else if (collapsedArrowDragStarted && moduleGesture != ModuleGesture::None) {
            UpdateModuleDrag(x, y);
        }
        return;
    }
    if (pickingScreenColor) {
        SampleScreenColorAtCursor();
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return;
    }
    if (draggingStudioColor || draggingStudioHue) {
        UpdateStudioColor(x, y, draggingStudioHue);
        return;
    }
    if (draggingLayer != 0) {
        UpdateLayerDrag(y);
        return;
    }
    if (songRowDragging) {
        UpdateSongRowFieldDrag(x, y);
        return;
    }
    if (draggingDecor != 0) {
        MoveDecor(x, y);
        return;
    }
    if (moduleGesture != ModuleGesture::None) {
        UpdateModuleDrag(x, y);
        return;
    }
    if (dragKind != DragKind::None) {
        UpdateRowDrag(x, y);
        return;
    }
    if (draggingSeek || draggingVolume) {
        const auto kind = draggingSeek ? HitKind::Seek : HitKind::Volume;
        for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator) {
            if (iterator->kind == kind) {
                SetSlider(*iterator, x);
                break;
            }
        }
    } else {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
    }
}

void Win32Ui::Impl::PointerUp(const std::optional<D2D1_POINT_2F> release) noexcept {
    if (pickingScreenColor) {
        // Button that starts the tool also raises WM_LBUTTONUP; ignore that first release.
        if (eyedropperSkipUp) {
            eyedropperSkipUp = false;
            return;
        }
        SampleScreenColorAtCursor();
        pickingScreenColor = false;
        if (previewPending) PushPreview();
        if (GetCapture() == window) ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    draggingSeek = false;
    draggingVolume = false;
    draggingDecor = 0;
    draggingStudioColor = false;
    draggingStudioHue = false;
    draggingLayer = 0;
    const bool songRowEdited = songRowDragging;
    if (release && songRowDragging) {
        UpdateSongRowFieldDrag(release->x, release->y);
        if (songRowDragMoved && !songRowResizing) (void)ApplySongRowSnap();
    }
    songRowDragging = false;
    songRowDragMoved = false;
    songRowResizing = false;
    if (!release && songRowEdited) songRowSnap.reset();
    if (collapsedArrowPress) {
        const auto id = collapsedArrowPress;
        const bool dragged = collapsedArrowDragStarted;
        collapsedArrowPress.reset();
        collapsedArrowDragStarted = false;
        collapsedArrowPressBounds = {};
        if (dragged && moduleGesture != ModuleGesture::None) {
            FinishModuleDrag();
        } else if (!dragged) {
            auto layout = model.moduleLayout;
            float resizedWidth = 0.0F;
            float resizedHeight = 0.0F;
            const bool wasCollapsed = layout.IsCollapsed(*id);
            const ModuleLayout layoutBeforeToggle = layout;
            const bool restoringExpansion = !wasCollapsed && moduleExpansionResizePending &&
                moduleExpansionResizeModule && *moduleExpansionResizeModule == *id;
            if (layout.ToggleCollapsedModule(*id, model.moduleExpansionBehavior,
                                             lastCanvas.width, lastCanvas.height,
                                             &resizedWidth, &resizedHeight)) {
                if (restoringExpansion) {
                    layout = moduleExpansionRestoreLayout;
                } else if (moduleExpansionResizePending) {
                    moduleExpansionResizePending = false;
                    moduleExpansionResizeModule.reset();
                }
                try { host.SetModuleLayout(layout); } catch (...) {}
                const bool resizedForExpansion = wasCollapsed &&
                    (resizedWidth > lastCanvas.width + 0.5F ||
                     resizedHeight > lastCanvas.height + 0.5F);
                if (resizedForExpansion) {
                    moduleExpansionResizePending = true;
                    moduleExpansionResizeModule = *id;
                    moduleExpansionRestoreLayout = layoutBeforeToggle;
                    moduleExpansionRestoreWidth = static_cast<int>(lastCanvas.width);
                    moduleExpansionRestoreHeight = static_cast<int>(lastCanvas.height);
                    internalModuleResize = true;
                    SetWindowPos(window, nullptr, 0, 0,
                                 static_cast<int>(std::ceil(std::max(resizedWidth, lastCanvas.width))),
                                 static_cast<int>(std::ceil(std::max(resizedHeight, lastCanvas.height) +
                                                            (HasTitlebar() ? kTitlebarHeight : 0.0F))),
                                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS);
                } else if (restoringExpansion) {
                    moduleExpansionResizePending = false;
                    moduleExpansionResizeModule.reset();
                    internalModuleResize = true;
                    SetWindowPos(window, nullptr, 0, 0,
                                 moduleExpansionRestoreWidth,
                                 moduleExpansionRestoreHeight +
                                     (HasTitlebar() ? static_cast<int>(kTitlebarHeight) : 0),
                                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS);
                }
            }
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (moduleGesture != ModuleGesture::None) FinishModuleDrag();
    if (dragKind != DragKind::None) {
        FinishRowDrag();
    }
    if (songRowEdited) CommitSongRowLayoutEdit();
    if (previewPending) PushPreview();
    if (GetCapture() == window) ReleaseCapture();
}

// Provides resize borders and a draggable caption for the borderless window.
[[nodiscard]] LRESULT Win32Ui::Impl::HitTestNonClient(int screenX, int screenY) const {
    RECT rc{};
    GetWindowRect(window, &rc);
    if (!IsZoomed(window)) {
        const bool onLeft = screenX < rc.left + kResizeBorder;
        const bool onRight = screenX >= rc.right - kResizeBorder;
        const bool onTop = screenY < rc.top + kResizeBorder;
        const bool onBottom = screenY >= rc.bottom - kResizeBorder;
        if (onTop && onLeft) return HTTOPLEFT;
        if (onTop && onRight) return HTTOPRIGHT;
        if (onBottom && onLeft) return HTBOTTOMLEFT;
        if (onBottom && onRight) return HTBOTTOMRIGHT;
        if (onTop) return HTTOP;
        if (onBottom) return HTBOTTOM;
        if (onLeft) return HTLEFT;
        if (onRight) return HTRIGHT;
    }
    // Inside the caption drag strip and not over an interactive control? Allow drag.
    POINT client{screenX, screenY};
    ScreenToClient(window, &client);
    if (Contains(captionRect, static_cast<float>(client.x), static_cast<float>(client.y))) {
        const bool onControl = std::any_of(titlebarControlBounds.begin(), titlebarControlBounds.end(),
                                           [client](const auto& bounds) {
                                               return Contains(bounds, static_cast<float>(client.x),
                                                               static_cast<float>(client.y));
                                           });
        if (!onControl) return HTCAPTION;
    }
    return HTCLIENT;
}

} // namespace rivan::ui
