// Win32Ui.Input.cpp
// Input / drag / keys / drop methods for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

// ---- Input ------------------------------------------------------------

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
        if (const HitRegion* collapseHit = HitTest(x, y);
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
    const HitRegion* found = HitTest(x, y);
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
                    const bool wasActive = tab == layout.activeTab;
                    layout.activeTab = tab;
                    try { host.SetModuleLayout(layout); } catch (...) {}
                    // A tab click still behaves like a normal selection when released
                    // without movement.  If the pointer moves, the same gesture can
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

void Win32Ui::Impl::PointerUp() noexcept {
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
            if (layout.ToggleCollapsedModule(*id)) {
                try { host.SetModuleLayout(layout); } catch (...) {}
            }
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (moduleGesture != ModuleGesture::None) FinishModuleDrag();
    if (dragKind != DragKind::None) {
        FinishRowDrag();
    }
    if (previewPending) PushPreview();
    if (GetCapture() == window) ReleaseCapture();
}

[[nodiscard]] bool Win32Ui::Impl::BeginModuleResize(float x, float y) {
    if (model.miniPlayer || lastCanvas.width <= 0.0F || lastCanvas.height <= 0.0F) return false;
    constexpr float edge = 7.0F;
    for (auto iterator = moduleRegions.rbegin(); iterator != moduleRegions.rend(); ++iterator) {
        if (!Contains(iterator->bounds, x, y)) continue;
        const bool right = std::abs(x - iterator->bounds.right) <= edge;
        const bool bottom = std::abs(y - iterator->bounds.bottom) <= edge;
        const bool left = std::abs(x - iterator->bounds.left) <= edge;
        const bool top = std::abs(y - iterator->bounds.top) <= edge;
        if (!right && !bottom && !left && !top) continue;
        // The tab strip is inset by four pixels from the panel edge.  Without
        // this guard its entire upper edge is interpreted as a resize handle,
        // so a tab can be selected but never promoted into a detachable drag.
         if (const auto* hit = HitTest(x, y);
             hit && (hit->kind == HitKind::ModuleTitle || hit->kind == HitKind::ModuleTab ||
                     hit->kind == HitKind::ModuleCollapseToggle)) {
            continue;
        }
        const auto geometryId = model.moduleLayout.TabRoot(iterator->id);
        const auto* item = model.moduleLayout.Find(geometryId);
        if (!item) return false;
        moduleLayoutDraft = model.moduleLayout;
        // Resize the visible rectangle, not the hidden rectangle belonging to the
        // currently selected tab.  This keeps all tabs at the same usable size.
        draggingModule = geometryId;
        moduleGesture = ModuleGesture::Resize;
        moduleDragActive = true;
        moduleResizeRight = right;
        moduleResizeBottom = bottom;
        moduleResizeLeft = left;
        moduleResizeTop = top;
        moduleDragStart = {x, y};
        ResetModuleDropPreview();
        SetCapture(window);
        return true;
    }
    return false;
}

void Win32Ui::Impl::BeginModuleDrag(ModuleId id, float x, float y,
                                     const ModuleLayout* layoutOverride,
                                     bool detachTabOnMove) {
    const ModuleLayout& sourceLayout = layoutOverride ? *layoutOverride : model.moduleLayout;
    if (!sourceLayout.HasValidGeometry()) return;
    const auto geometryId = sourceLayout.TabRoot(id);
    const auto* sourceItem = sourceLayout.Find(geometryId);
    if (!sourceItem) return;
    if (sourceLayout.IsTabbed(id) && sourceLayout.Find(id) == nullptr) return;
    moduleLayoutDraft = sourceLayout;
    moduleDragFromCollapsedArrow = false;
    moduleCollapsedArrowOrigin = {};
    if (auto* collapsed = moduleLayoutDraft.Find(id); collapsed && collapsed->collapsed &&
        collapsed->expandedWidth >= 0.10F && collapsed->expandedHeight >= 0.10F) {
        collapsed->x = collapsed->expandedX;
        collapsed->y = collapsed->expandedY;
        collapsed->width = collapsed->expandedWidth;
        collapsed->height = collapsed->expandedHeight;
        collapsed->collapsed = false;
    }
    if (const auto* restored = moduleLayoutDraft.Find(id); restored && restored->collapseMode != ModuleCollapseMode::None) {
        if (auto* mutableRestored = moduleLayoutDraft.Find(id)) {
            mutableRestored->collapseMode = ModuleCollapseMode::None;
            mutableRestored->collapseSide = ModuleCollapseSide::None;
            mutableRestored->collapseTarget = id;
            mutableRestored->collapseTargetIsWindow = false;
        }
    }
    // The source-layout pointer may still refer to the collapsed handle. Use the
    // draft's restored rectangle for the drag offset so the first motion does not jump.
    const auto* item = moduleLayoutDraft.Find(geometryId);
    if (!item) return;
    // A tab may have retained its old standalone rectangle.  Seed the drag from
    // the rectangle the user can actually see so it does not jump or appear tiny
    // when it is detached from the group.
    if (geometryId != id) {
        if (auto* dragged = moduleLayoutDraft.Find(id)) {
            dragged->x = item->x;
            dragged->y = item->y;
            dragged->width = item->width;
            dragged->height = item->height;
        }
    }
    draggingModule = id;
    moduleGesture = ModuleGesture::Move;
    moduleDragActive = false;
    moduleDetachTabOnMove = detachTabOnMove;
    moduleMoveTabbedGroup = false;
    moduleMoveSnapGroup = moduleLayoutDraft.IsSnapped(id) && moduleLayoutDraft.IsSnapGrouped(id) &&
                          moduleLayoutDraft.SnapRoot(id) == id;
    moduleDragSnapRoot = moduleLayoutDraft.SnapRoot(id);
    moduleDragStart = {x, y};
    moduleDragOffset = {x - item->x * lastCanvas.width, y - item->y * lastCanvas.height};
    ResetModuleDropPreview();
    SetCapture(window);
}

void Win32Ui::Impl::UpdateModuleDrag(float x, float y) {
    if (!draggingModule || lastCanvas.width <= 0.0F || lastCanvas.height <= 0.0F) return;
    if (!moduleDragActive) {
        const float dx = x - moduleDragStart.x;
        const float dy = y - moduleDragStart.y;
        if (dx * dx + dy * dy < 25.0F) return;
        moduleDragActive = true;
    }
    const auto draggedId = *draggingModule;
    if (moduleGesture == ModuleGesture::Move) {
        moduleLayoutDraft.ClearCollapseReferences(draggedId);
        moduleLayoutDraft.ClearModuleCollapse(draggedId);
    }
    if (moduleGesture == ModuleGesture::Move && moduleDetachTabOnMove &&
        moduleLayoutDraft.IsTabbed(draggedId)) {
        // Detach as soon as the movement threshold is crossed.  The detached panel
        // then follows the pointer during the drag, while a drop on another panel
        // can re-create the tab group in FinishModuleDrag.
        DetachModuleFromTabs(moduleLayoutDraft, draggedId);
    }
    if (moduleGesture == ModuleGesture::Move && !moduleDetachTabOnMove) {
        moduleMoveTabbedGroup = moduleLayoutDraft.IsTabbed(draggedId);
        moduleMoveSnapGroup = moduleLayoutDraft.IsSnapped(draggedId) &&
                              moduleLayoutDraft.IsSnapGrouped(draggedId) &&
                              moduleLayoutDraft.SnapRoot(draggedId) == draggedId;
        moduleDragSnapRoot = moduleLayoutDraft.SnapRoot(draggedId);
    }
    auto* item = moduleLayoutDraft.Find(draggedId);
    if (!item) return;
    if (moduleGesture == ModuleGesture::Resize) {
        if (moduleLayoutDraft.IsSnapGrouped(draggedId)) {
            moduleLayoutDraft.ResizeSnapGroup(draggedId,
                                              x / lastCanvas.width, y / lastCanvas.height,
                                              moduleResizeRight, moduleResizeBottom,
                                              moduleResizeLeft, moduleResizeTop);
        } else {
            if (moduleResizeLeft) {
                const float right = item->x + item->width;
                item->x = std::clamp(x / lastCanvas.width, 0.0F, right - 0.10F);
                item->width = right - item->x;
            }
            if (moduleResizeTop) {
                const float bottom = item->y + item->height;
                item->y = std::clamp(y / lastCanvas.height, 0.0F, bottom - 0.10F);
                item->height = bottom - item->y;
            }
            if (moduleResizeRight) {
                item->width = std::clamp((x - item->x * lastCanvas.width) / lastCanvas.width,
                                         0.10F, 1.0F - item->x);
            }
            if (moduleResizeBottom) {
                item->height = std::clamp((y - item->y * lastCanvas.height) / lastCanvas.height,
                                           0.10F, 1.0F - item->y);
            }
        }
        ModuleLayout::SyncExpandedGeometry(*item);
    } else {
        if (item->collapsed) {
            item->x = item->expandedX;
            item->y = item->expandedY;
            item->width = item->expandedWidth;
            item->height = item->expandedHeight;
            item->collapsed = false;
        }
        const float nextX = std::clamp((x - moduleDragOffset.x) / lastCanvas.width,
                                       0.0F, 1.0F - item->width);
        const float nextY = std::clamp((y - moduleDragOffset.y) / lastCanvas.height,
                                       0.0F, 1.0F - item->height);
        if (moduleMoveSnapGroup) {
            const float deltaX = nextX - item->x;
            const float deltaY = nextY - item->y;
            const auto tabRoot = moduleLayoutDraft.TabRoot(draggedId);
            for (auto& member : moduleLayoutDraft.items) {
                const bool inSnapGroup = moduleLayoutDraft.SnapRoot(member.id) == moduleDragSnapRoot;
                const bool inTabGroup = moduleLayoutDraft.IsTabbed(member.id) &&
                                        moduleLayoutDraft.TabRoot(member.id) == tabRoot;
                if (inSnapGroup || inTabGroup) {
                    member.x = std::clamp(member.x + deltaX, 0.0F, 1.0F - member.width);
                    member.y = std::clamp(member.y + deltaY, 0.0F, 1.0F - member.height);
                }
            }
        } else if (moduleMoveTabbedGroup) {
            const float deltaX = nextX - item->x;
            const float deltaY = nextY - item->y;
            for (std::size_t i = 0; i < moduleLayoutDraft.TabCount(); ++i) {
                if (auto* member = moduleLayoutDraft.Find(moduleLayoutDraft.tabOrder[i])) {
                    member->x = std::clamp(member->x + deltaX, 0.0F, 1.0F - member->width);
                    member->y = std::clamp(member->y + deltaY, 0.0F, 1.0F - member->height);
                }
            }
        } else {
            // Picking up a standalone snapped member detaches it from its snap
            // group and leaves it floating until another side drop.
            if (moduleLayoutDraft.IsSnapped(draggedId)) {
                moduleLayoutDraft.DetachSnapModule(draggedId);
            }
            item = moduleLayoutDraft.Find(draggedId);
            if (!item) return;
            item->dockState = ModuleDockState::Floating;
            item->x = nextX;
            item->y = nextY;
        }
        ResolveModuleDropPreview(x, y);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::ResetModuleDropPreview() noexcept {
    moduleDropTarget.reset();
    moduleDropZone = ModuleDropZone::None;
    moduleWindowDropZone = ModuleWindowDropZone::None;
    moduleCollapseTarget.reset();
    moduleCollapseSide = ModuleCollapseSide::None;
    moduleCollapseMode = ModuleCollapseMode::None;
    moduleCollapseTargetIsWindow = false;
    moduleDropPreviewValid = false;
    moduleLayoutPreview = moduleLayoutDraft;
    moduleDropLastPointer = {-1.0F, -1.0F};
}

void Win32Ui::Impl::ResolveModuleDropPreview(float x, float y) {
    if (!draggingModule || moduleGesture != ModuleGesture::Move ||
        !moduleDragActive || lastCanvas.width <= 0.0F || lastCanvas.height <= 0.0F) {
        ResetModuleDropPreview();
        return;
    }

    std::optional<ModuleId> targetModule;
    ModuleDropZone targetZone = ModuleDropZone::None;
     const auto boundsFor = [this](const ModuleLayoutItem& item) {
         return ModulePixelBounds(item, lastCanvas);
     };
    // Resolve against the draft, rather than the last painted hit regions.  This
    // remains correct while the idle preview is showing a different layout.
    for (auto iterator = moduleLayoutDraft.items.rbegin();
         iterator != moduleLayoutDraft.items.rend(); ++iterator) {
        if (!iterator->visible || iterator->collapsed || iterator->id == *draggingModule) continue;
        if (moduleLayoutDraft.SnapRoot(iterator->id) ==
            moduleLayoutDraft.SnapRoot(*draggingModule)) continue;
        if (moduleLayoutDraft.IsTabbed(*draggingModule) &&
            moduleLayoutDraft.IsTabbed(iterator->id) &&
            moduleLayoutDraft.TabRoot(iterator->id) == moduleLayoutDraft.TabRoot(*draggingModule)) {
            continue;
        }
        if (moduleLayoutDraft.IsTabbed(iterator->id)) {
            const auto activeTab = moduleLayoutDraft.tabOrder[moduleLayoutDraft.ActiveTabIndex()];
            if (activeTab != iterator->id) continue;
        }
        const auto* geometry = moduleLayoutDraft.Find(moduleLayoutDraft.TabRoot(iterator->id));
        if (!geometry) continue;
        const auto zone = ResolveModuleDropZone(x, y, boundsFor(*geometry).left,
                                                boundsFor(*geometry).top,
                                                boundsFor(*geometry).right,
                                                boundsFor(*geometry).bottom);
        if (zone != ModuleDropZone::None) {
            targetModule = iterator->id;
            targetZone = zone;
            break;
        }
    }

    ModuleWindowDropZone windowZone = ModuleWindowDropZone::None;
    std::optional<ModuleId> collapseTarget;
    ModuleCollapseSide collapseSide = ModuleCollapseSide::None;
    ModuleCollapseMode collapseMode = ModuleCollapseMode::None;
    bool collapseTargetIsWindow = false;
    ModuleLayout candidate = moduleLayoutDraft;
    bool previewCanApply = false;

    // These strips are intentionally narrow so existing side/corner snapping wins
    // everywhere except immediately beside an edge or module edge.
    const auto edgeSide = [&]() {
        if (x <= kModuleCollapseZonePixels) return ModuleCollapseSide::Left;
        if (x >= lastCanvas.width - kModuleCollapseZonePixels) return ModuleCollapseSide::Right;
        if (y <= kModuleCollapseZonePixels) return ModuleCollapseSide::Top;
        if (y >= lastCanvas.height - kModuleCollapseZonePixels) return ModuleCollapseSide::Bottom;
        return ModuleCollapseSide::None;
    };
    if (const auto side = edgeSide(); side != ModuleCollapseSide::None &&
        candidate.CollapseToWindow(*draggingModule, side)) {
        targetModule.reset();
        targetZone = ModuleDropZone::None;
        windowZone = ModuleWindowDropZone::None;
        collapseSide = side;
        collapseMode = ModuleCollapseMode::Outside;
        collapseTargetIsWindow = true;
        previewCanApply = true;
    }

    const auto resolveCollapse = [this, x, y](const ModuleLayoutItem& item,
                                              ModuleCollapseSide& side,
                                              ModuleCollapseMode& mode) {
        const auto bounds = ModuleRawPixelBounds(item, lastCanvas);
        const float strip = kModuleCollapseZonePixels;
        const bool middleY = y >= bounds.top + Height(bounds) * 0.25F &&
                             y <= bounds.bottom - Height(bounds) * 0.25F;
        const bool middleX = x >= bounds.left + Width(bounds) * 0.25F &&
                             x <= bounds.right - Width(bounds) * 0.25F;
        if (middleY && x >= bounds.left - strip && x <= bounds.left + strip) {
            side = ModuleCollapseSide::Left;
            mode = x < bounds.left ? ModuleCollapseMode::Outside : ModuleCollapseMode::Inside;
            return true;
        }
        if (middleY && x >= bounds.right - strip && x <= bounds.right + strip) {
            side = ModuleCollapseSide::Right;
            mode = x > bounds.right ? ModuleCollapseMode::Outside : ModuleCollapseMode::Inside;
            return true;
        }
        if (middleX && y >= bounds.top - strip && y <= bounds.top + strip) {
            side = ModuleCollapseSide::Top;
            mode = y < bounds.top ? ModuleCollapseMode::Outside : ModuleCollapseMode::Inside;
            return true;
        }
        if (middleX && y >= bounds.bottom - strip && y <= bounds.bottom + strip) {
            side = ModuleCollapseSide::Bottom;
            mode = y > bounds.bottom ? ModuleCollapseMode::Outside : ModuleCollapseMode::Inside;
            return true;
        }
        return false;
    };
    if (!previewCanApply) {
        for (auto iterator = moduleLayoutDraft.items.rbegin();
             iterator != moduleLayoutDraft.items.rend(); ++iterator) {
            if (!iterator->visible || iterator->id == *draggingModule || iterator->collapsed) continue;
            if (moduleDragFromCollapsedArrow && Contains(moduleCollapsedArrowOrigin, x, y)) continue;
            if (moduleLayoutDraft.SnapRoot(iterator->id) ==
                moduleLayoutDraft.SnapRoot(*draggingModule)) continue;
            ModuleCollapseSide side = ModuleCollapseSide::None;
            ModuleCollapseMode mode = ModuleCollapseMode::None;
            if (!resolveCollapse(*iterator, side, mode)) continue;
            candidate = moduleLayoutDraft;
            if (!candidate.CollapseToModule(*draggingModule, iterator->id, side, mode)) continue;
            targetModule.reset();
            targetZone = ModuleDropZone::None;
            windowZone = ModuleWindowDropZone::None;
            collapseTarget = iterator->id;
            collapseSide = side;
            collapseMode = mode;
            previewCanApply = true;
            break;
        }
    }

    if (!previewCanApply) {
    if (targetModule) {
        if (targetZone == ModuleDropZone::Center) {
            candidate.TabWith(*draggingModule, *targetModule);
            previewCanApply = candidate.IsTabbed(*draggingModule);
        } else {
            previewCanApply = candidate.SnapTo(*draggingModule, *targetModule, targetZone);
        }
        // A module can be visually close to a target whose split is already too
        // small.  In that case try a window target instead of showing an invalid
        // white box over a geometry we cannot commit.
        if (!previewCanApply) {
            targetModule.reset();
            targetZone = ModuleDropZone::None;
        }
    }
    if (!targetModule) {
        windowZone = ResolveModuleWindowDropZone(
            x / lastCanvas.width, y / lastCanvas.height);
        if (windowZone != ModuleWindowDropZone::None) {
            candidate = moduleLayoutDraft;
            previewCanApply = candidate.SnapToWindow(*draggingModule, windowZone,
                                                     x / lastCanvas.width, y / lastCanvas.height);
            if (!previewCanApply) windowZone = ModuleWindowDropZone::None;
        }
    }
    }

    moduleDropLastPointer = {x, y};
    if (!targetModule && windowZone == ModuleWindowDropZone::None && !collapseTarget &&
        collapseMode == ModuleCollapseMode::None && !collapseTargetIsWindow) {
        ResetModuleDropPreview();
        return;
    }
    moduleDropTarget = targetModule;
    moduleDropZone = targetZone;
    moduleWindowDropZone = windowZone;
    moduleCollapseTarget = collapseTarget;
    moduleCollapseSide = collapseSide;
    moduleCollapseMode = collapseMode;
    moduleCollapseTargetIsWindow = collapseTargetIsWindow;
    moduleLayoutPreview = candidate;
    moduleDropPreviewValid = previewCanApply;
}

void Win32Ui::Impl::FinishModuleDrag() noexcept {
    const auto dragged = draggingModule;
    const bool active = moduleDragActive;
    const bool moving = moduleGesture == ModuleGesture::Move;
    const auto drop = moduleDropTarget;
    const auto zone = moduleDropZone;
    const auto windowDrop = moduleWindowDropZone;
    const auto collapseDrop = moduleCollapseTarget;
    const auto collapseSideDrop = moduleCollapseSide;
    const auto collapseModeDrop = moduleCollapseMode;
    const bool collapseWindowDrop = moduleCollapseTargetIsWindow;
    const bool previewValid = moduleDropPreviewValid;
    const bool detachTabOnMove = moduleDetachTabOnMove;
    const bool moveTabbedGroup = moduleMoveTabbedGroup;
    const bool moveSnapGroup = moduleMoveSnapGroup;
    moduleGesture = ModuleGesture::None;
    moduleDragActive = false;
    draggingModule.reset();
    moduleDropTarget.reset();
    moduleDropZone = ModuleDropZone::None;
    moduleWindowDropZone = ModuleWindowDropZone::None;
    moduleCollapseTarget.reset();
    moduleCollapseSide = ModuleCollapseSide::None;
    moduleCollapseMode = ModuleCollapseMode::None;
    moduleCollapseTargetIsWindow = false;
    moduleDragFromCollapsedArrow = false;
    moduleCollapsedArrowOrigin = {};
    moduleDropPreviewValid = false;
    moduleResizeRight = false;
    moduleResizeBottom = false;
    moduleResizeLeft = false;
    moduleResizeTop = false;
    moduleDetachTabOnMove = false;
    moduleMoveTabbedGroup = false;
    moduleMoveSnapGroup = false;
    if (dragged && active) {
        if (moving && ((drop && *drop != *dragged && zone != ModuleDropZone::None) ||
                      windowDrop != ModuleWindowDropZone::None ||
                      collapseModeDrop != ModuleCollapseMode::None || collapseWindowDrop)) {
            if (previewValid && (windowDrop != ModuleWindowDropZone::None ||
                                 zone != ModuleDropZone::None ||
                                 collapseModeDrop != ModuleCollapseMode::None || collapseWindowDrop)) {
                moduleLayoutDraft = moduleLayoutPreview;
            } else if (collapseModeDrop != ModuleCollapseMode::None) {
                if (collapseWindowDrop) {
                    (void)moduleLayoutDraft.CollapseToWindow(*dragged, collapseSideDrop);
                } else if (collapseDrop) {
                    (void)moduleLayoutDraft.CollapseToModule(*dragged, *collapseDrop,
                                                              collapseSideDrop, collapseModeDrop);
                }
            } else if (zone == ModuleDropZone::Center) {
                moduleLayoutDraft.TabWith(*dragged, *drop);
            } else if (windowDrop != ModuleWindowDropZone::None) {
                (void)moduleLayoutDraft.SnapToWindow(
                    *dragged, windowDrop, moduleDropLastPointer.x / lastCanvas.width,
                    moduleDropLastPointer.y / lastCanvas.height);
            } else {
                (void)moduleLayoutDraft.SnapTo(*dragged, *drop, zone);
            }
        } else if (moving) {
            if (detachTabOnMove || (!moveTabbedGroup && !moveSnapGroup)) {
                DetachModuleFromTabs(moduleLayoutDraft, *dragged);
            }
            if (auto* item = moduleLayoutDraft.Find(*dragged)) {
                if (detachTabOnMove || (!moveTabbedGroup && !moveSnapGroup)) {
                    item->dockState = ModuleDockState::Floating;
                }
            }
        }
        try { host.SetModuleLayout(moduleLayoutDraft); } catch (...) {}
    }
    if (GetCapture() == window) ReleaseCapture();
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::DetachModuleFromTabs(ModuleLayout& layout, ModuleId id) const noexcept {
    if (!layout.IsTabbed(id)) return;
    const auto* root = layout.Find(layout.TabRoot(id));
    if (!root) return;
    const auto geometry = *root;
    // When the root tab is removed, the remaining tab must keep the group's
    // visible rectangle instead of reverting to its old hidden rectangle.
    for (std::size_t index = 0; index < layout.TabCount(); ++index) {
        const auto tab = layout.tabOrder[index];
        if (tab == id) continue;
        if (auto* item = layout.Find(tab)) {
            item->x = geometry.x;
            item->y = geometry.y;
            item->width = geometry.width;
            item->height = geometry.height;
        }
    }
    layout.RemoveTab(id);
}

[[nodiscard]] HCURSOR Win32Ui::Impl::ModuleCursor(float x, float y) const noexcept {
    if (windowKind != WindowKind::Main || model.miniPlayer) return nullptr;
    constexpr float edge = 7.0F;
    for (auto iterator = moduleRegions.rbegin(); iterator != moduleRegions.rend(); ++iterator) {
        if (!Contains(iterator->bounds, x, y)) continue;
        const bool right = std::abs(x - iterator->bounds.right) <= edge;
        const bool bottom = std::abs(y - iterator->bounds.bottom) <= edge;
        const bool left = std::abs(x - iterator->bounds.left) <= edge;
        const bool top = std::abs(y - iterator->bounds.top) <= edge;
        if (right && bottom) return LoadCursorW(nullptr, IDC_SIZENWSE);
        if (left && top) return LoadCursorW(nullptr, IDC_SIZENWSE);
        if ((right && top) || (left && bottom)) return LoadCursorW(nullptr, IDC_SIZENESW);
        if (right) return LoadCursorW(nullptr, IDC_SIZEWE);
        if (left) return LoadCursorW(nullptr, IDC_SIZEWE);
        if (bottom) return LoadCursorW(nullptr, IDC_SIZENS);
        if (top) return LoadCursorW(nullptr, IDC_SIZENS);
    }
    if (moduleGesture == ModuleGesture::Move) return LoadCursorW(nullptr, IDC_SIZEALL);
    return nullptr;
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
    if (Contains(captionRect, static_cast<float>(client.x), static_cast<float>(client.y)) &&
        HitTest(static_cast<float>(client.x), static_cast<float>(client.y)) == nullptr) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

[[nodiscard]] bool Win32Ui::Impl::IsImageDrop(const std::filesystem::path& path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".webp";
}

void Win32Ui::Impl::DropFiles(HDROP drop) {
    POINT dropPoint{};
    DragQueryPoint(drop, &dropPoint);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
    std::vector<std::wstring> audioPaths;
    std::vector<std::wstring> imagePaths;
    audioPaths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1U, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1U);
        path.resize(length);
        if (windowKind == WindowKind::Main && model.skinStudioVisible && IsImageDrop(path)) {
            imagePaths.push_back(std::move(path));
        }
        else audioPaths.push_back(std::move(path));
    }
    DragFinish(drop);

    // Image files dropped while the studio is open become positioned skin images at
    // the drop location; everything else is enqueued as audio as before.
    if (!imagePaths.empty()) AddDroppedImages(imagePaths, dropPoint);
    if (!audioPaths.empty()) {
        try { host.ImportDroppedFiles(audioPaths); } catch (...) {}
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::AddDroppedImages(const std::vector<std::wstring>& paths, POINT dropPoint) {
    if (windowKind == WindowKind::Main) studioDraft = model.activeSkin;
    float nx = 0.35F;
    float ny = 0.35F;
    if (lastCanvas.width > 0.0F && lastCanvas.height > 0.0F) {
        nx = std::clamp(static_cast<float>(dropPoint.x) / lastCanvas.width, 0.0F, 0.9F);
        ny = std::clamp(static_cast<float>(dropPoint.y) / lastCanvas.height, 0.0F, 0.9F);
    }
    bool changed = false;
    for (const auto& path : paths) {
        if (studioDraft.images.size() >= 32) break;
        std::wstring error;
        const auto relative = host.ImportSkinAsset(studioDraft.id, std::filesystem::path(path),
                                                   SkinAssetKind::BackgroundImage, error);
        if (!relative) {
            if (!error.empty()) MessageBoxW(window, error.c_str(), L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
            continue;
        }
        skin::SkinImage image;
        image.file = *relative;
        image.x = nx;
        image.y = ny;
        studioDraft.images.push_back(std::move(image));
        nx = std::clamp(nx + 0.05F, 0.0F, 0.9F);
        ny = std::clamp(ny + 0.05F, 0.0F, 0.9F);
        changed = true;
    }
    if (changed) PushPreview();
}

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
    for (const auto& track : model.tracks) {
        if (Matches(track, playlistQuery)) {
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

void Win32Ui::Impl::KeyDown(WPARAM key) {
    if (previewFullscreen && windowKind == WindowKind::Main) {
        if (key == VK_ESCAPE) {
            ExitPreviewFullscreen();
            return;
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
        return;
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
        return;
    }
    if (playlistNameEditing) {
        if (key == VK_RETURN) CommitPlaylistName();
        else if (key == VK_ESCAPE) CancelPlaylistName();
        return;
    }
    if (pickingScreenColor) {
        if (key == VK_ESCAPE) CancelScreenEyedropper();
        return;
    }
    if (studioNameEditing || managerNameEditing) {
        if (key == VK_RETURN) HandleStudioAction(studioNameEditing ? 96 : 900 + managerSkinIndex);
        else if (key == VK_ESCAPE) {
            studioNameEditing = false;
            managerNameEditing = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
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
        return;
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
            return;
        } else if (control && (key == L'V' || key == L'v')) {
            PastePlaylistQuery();
            return;
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (windowKind == WindowKind::Main && !model.miniPlayer && key == VK_F2 && !trackSelection.empty()) {
        const auto chosen = trackSelection.contains(trackAnchor) ? trackAnchor : *trackSelection.begin();
        BeginTrackRename(chosen);
        return;
    }
    if (windowKind == WindowKind::Main && !model.miniPlayer && key == VK_DELETE && !trackSelection.empty()) {
        RemoveSelectedTracks();
        return;
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
}

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

// ---- Multi-select, drag reorder, context menus, inline naming ---------------


// True when a tree row can be drag-reordered: any Directory folder or User playlist.

// Sibling-group key used to scope a folder/playlist reorder. Directory folders carry
// their manager parentId; User playlists carry kUserPlaylistGroupParent. Returns a
// sentinel that matches nothing (0 is a valid parentId for scan roots) when not found.



// Click selection for a track row. Plain click = single-select; ctrl = toggle; shift =
// range from the anchor. Keyed by stable model index so duplicates stay distinct.

// Click selection for a tree playlist row. Ctrl/shift build a multi-selection among
// user playlists only (generated rows cannot be reordered/deleted in bulk).





// Maps a pointer Y to a drop position (index into the dragged row's source playlist).
// Uses the row midpoint so dropping on the upper half lands before, lower half after.











// Right-click: context menu on a track or a tree playlist row.

// TrackPopupMenu runs a nested loop; after it returns the cursor may have moved without
// any WM_MOUSEMOVE to this window. Resync hover coords so rows do not stay "hot".



} // namespace rivan::ui
