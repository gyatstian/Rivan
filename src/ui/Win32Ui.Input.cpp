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
    // Studio edits decor on the main canvas. Main-player buttons and track rows must
    // not steal clicks from an image/shape while the pointer is outside the studio.
    if (windowKind == WindowKind::Main && model.skinStudioVisible && BeginDecorDrag(x, y)) return;
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
    if (hit.kind != HitKind::DiscordImageField && discordImageEditing) {
        CommitDiscordImageUrl();
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
        case HitKind::FilePreviewToggle:
            filePreviewExpanded = !filePreviewExpanded;
            if (!filePreviewExpanded) ClearFilePreview();
            else LoadFilePreview(ActivePreviewPath());
            break;
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
        case HitKind::DiscordImageField:
            if (!discordImageEditing) discordImageBuffer = model.discordImageUrl;
            discordImageEditing = true;
            discordImageSelectAll = !discordImageBuffer.empty();
            SetFocus(window);
            break;
        case HitKind::WindowControl:
            if (hit.id == 1) ShowWindow(window, SW_MINIMIZE);
            else if (hit.id == 2) host.Invoke(Command::ToggleMiniPlayer);
            else if (hit.id == 3) PostMessageW(window, WM_CLOSE, 0, 0);
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
    if (dragKind != DragKind::None) {
        FinishRowDrag();
    }
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
    if (discordImageEditing) {
        if (character != L'\b' && (character < L' ' || character == 0x7FU)) return;
        if (character == L'\b') {
            if (discordImageSelectAll) discordImageBuffer.clear();
            else if (!discordImageBuffer.empty()) discordImageBuffer.pop_back();
        } else {
            if (discordImageSelectAll) discordImageBuffer.clear();
            if (discordImageBuffer.size() < 2048) discordImageBuffer.push_back(character);
        }
        discordImageSelectAll = false;
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
    if (discordImageEditing) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (key == VK_ESCAPE) {
            discordImageEditing = false;
            discordImageSelectAll = false;
            discordImageBuffer.clear();
        } else if (key == VK_RETURN) {
            CommitDiscordImageUrl();
            return;
        } else if (control && (key == L'A' || key == L'a')) {
            discordImageSelectAll = !discordImageBuffer.empty();
        } else if (control && (key == L'V' || key == L'v')) {
            PasteDiscordImageUrl();
            return;
        }
        InvalidateRect(window, nullptr, FALSE);
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

bool Win32Ui::Impl::IsUserPlaylistId(std::uint64_t id) const noexcept {
    for (const auto& playlist : model.playlists) {
        if (playlist.id == id) return playlist.user;
    }
    return false;
}

// True when a tree row can be drag-reordered: any Directory folder or User playlist.
bool Win32Ui::Impl::IsReorderablePlaylistId(std::uint64_t id) const noexcept {
    for (const auto& playlist : model.playlists) {
        if (playlist.id == id) return playlist.reorderable;
    }
    return false;
}

// Sibling-group key used to scope a folder/playlist reorder. Directory folders carry
// their manager parentId; User playlists carry kUserPlaylistGroupParent. Returns a
// sentinel that matches nothing (0 is a valid parentId for scan roots) when not found.
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

// Click selection for a track row. Plain click = single-select; ctrl = toggle; shift =
// range from the anchor. Keyed by stable model index so duplicates stay distinct.
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

// Click selection for a tree playlist row. Ctrl/shift build a multi-selection among
// user playlists only (generated rows cannot be reordered/deleted in bulk).
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
        // Track reorder is allowed on user playlists and Directory folders.
        if (dragKind == DragKind::Track && !model.selectedPlaylistTracksReorderable) return;
        // A Directory view also shows recursive child-folder sections. Only its direct
        // tracks belong to the selected folder's reorderable track list.
        if (dragKind == DragKind::Track && !model.trackSections.empty() &&
            (model.trackSections.front().label.empty()
                 ? dragTrackIndex >= model.trackSections.front().count
                 : true)) return;
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
    dropBeforePlaylistId = 0;
    dropIntoPlaylistId = 0;
    dropAtPlaylistEnd = false;
    InvalidateRect(window, nullptr, FALSE);
}

// Maps a pointer Y to a drop position (index into model.tracks; size() == append).
// Uses the row midpoint so dropping on the upper half lands before, lower half after.
std::size_t Win32Ui::Impl::ResolveTrackDrop(float y) const {
    // Use rendered hit rows instead of playlistListBounds: Directory folders render in
    // the current-folder pane, while user playlists render in the editor pane.
    if (model.tracks.empty()) return 0;
    const std::size_t directCount = model.trackSections.empty()
                                        ? model.tracks.size()
                                        : (model.trackSections.front().label.empty()
                                               ? model.trackSections.front().count
                                               : 0);
    const HitRegion* first = nullptr;
    const HitRegion* last = nullptr;
    for (const auto& hit : hits) {
        // Both current-folder and playlist-editor panes can contribute track hits. Restrict
        // this drag to its original pane; otherwise a same-height row in the other pane
        // can replace the correct insertion target after a redraw.
        if (hit.kind != HitKind::Track || hit.index >= directCount ||
            dragStart.x < hit.bounds.left || dragStart.x >= hit.bounds.right) continue;
        if (first == nullptr || hit.bounds.top < first->bounds.top) first = &hit;
        if (last == nullptr || hit.bounds.top > last->bounds.top) last = &hit;
        if (y >= hit.bounds.top && y < hit.bounds.bottom) {
            return y > (hit.bounds.top + hit.bounds.bottom) * 0.5F
                       ? hit.index + 1
                       : hit.index;
        }
    }
    if (first == nullptr) return 0;
    if (y <= first->bounds.top) return first->index;
    return last->index + 1;
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
    auto indices = SelectedTrackIndicesSorted();
    if (indices.empty()) return;
    host.ReorderSelectedTracks(indices, dropTrackIndex);
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

// Right-click: context menu on a track or a tree playlist row.
void Win32Ui::Impl::PointerRightDown(float x, float y) {
    if (windowKind != WindowKind::Main || model.miniPlayer) return;
    SetFocus(window);
    // Menus depend on playlist permissions; repaint can lag a recent selection.
    try { host.SnapshotUiModel(model); } catch (...) {}
    mouse = {static_cast<LONG>(x), static_cast<LONG>(y)};
    const HitRegion* found = HitTest(x, y);
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

// TrackPopupMenu runs a nested loop; after it returns the cursor may have moved without
// any WM_MOUSEMOVE to this window. Resync hover coords so rows do not stay "hot".
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
