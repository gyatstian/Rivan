// Win32Ui.Paint.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::SyncRefreshTimer() noexcept {
        if (!window || windowKind != WindowKind::Main) return;
        if (!IsWindowVisible(window) || IsIconic(window)) {
            if (currentTimerMs != 0) KillTimer(window, kRefreshTimer);
            currentTimerMs = 0;
            return;
        }
        // Video preview needs ~30 Hz while expanded even if transport is idle.
        const UINT desired = (moduleGesture != ModuleGesture::None ||
                              model.playback == PlaybackState::Playing ||
                              (IsVideoPreviewModuleVisible() && previewIsVideo) || previewFullscreen)
            ? kRefreshPlayingMilliseconds
            : kRefreshIdleMilliseconds;
        if (desired == currentTimerMs) return;
        currentTimerMs = desired;
        SetTimer(window, kRefreshTimer, currentTimerMs, nullptr);
    }

[[nodiscard]] bool Win32Ui::Impl::HasTitlebar() const noexcept {
    return !(windowKind == WindowKind::Main && previewFullscreen &&
             previewIsVideo && IsVideoPreviewModuleVisible());
}

void Win32Ui::Impl::Paint() {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        if (IsIconic(window)) {
            EndPaint(window, &paint);
            SyncRefreshTimer();
            return;
        }
        if (!CreateTarget()) {
            EndPaint(window, &paint);
            return;
        }
        const std::uint64_t previousRevision = model.revision;
        try { host.SnapshotUiModel(model); } catch (...) {}
        if (!model.trackCoverArtEnabled && !trackCoverCache.empty()) {
            trackCoverCache.clear();
            trackCoverUseCounter = 0;
            nextTrackCoverLookup = {};
        }
        // Paint only presents latest decoded preview frame. Decoder work stays off UI thread.
        if (windowKind == WindowKind::Main) SyncFilePreview();
        Win32Ui::Impl::SyncSelectionToPlayback();
        Win32Ui::Impl::ApplySkinFonts();
        hits.clear();
        colorFocusRegions.clear();
        moduleRegions.clear();
        auto& brushes = UpdateBrushes();
        target->BeginDraw();
        const auto size = target->GetSize();
        const bool fullscreen = previewFullscreen && previewIsVideo && IsVideoPreviewModuleVisible();
        const bool hasTitlebar = !fullscreen;
        const D2D1_SIZE_F canvasSize{
            size.width, std::max(1.0F, size.height - (hasTitlebar ? kTitlebarHeight : 0.0F))};
        lastCanvas = canvasSize;
        if (windowKind == WindowKind::Settings) {
            target->SetTransform(D2D1::Matrix3x2F::Translation(0.0F, kTitlebarHeight));
            Win32Ui::Impl::DrawSettings(canvasSize, brushes);
            target->SetTransform(D2D1::Matrix3x2F::Identity());
            Win32Ui::Impl::DrawTitlebar(size, brushes);
        } else if (windowKind == WindowKind::SkinStudio) {
            if (studioSection == StudioSection::Colors &&
                model.skinColorFocusRevision != seenColorFocusRevision) {
                studioColorIndex = std::min(model.focusedSkinColor, StudioColorFields().size() - 1);
                studioColorPickerVisible = true;
                studioHexEditing = false;
                studioHexSelectAll = false;
                seenColorFocusRevision = model.skinColorFocusRevision;
            }
            if (model.skinElementFocusRevision != seenElementFocusRevision) {
                const int focused = model.focusedSkinElement;
                if (focused != 0) studioSection = StudioSection::Elements;
                studioShapeFocused = focused > 0;
                studioImageFocused = focused < 0;
                if (focused > 0 && !model.activeSkin.shapes.empty()) {
                    studioShapeIndex = std::min(static_cast<std::size_t>(focused - 1),
                                                model.activeSkin.shapes.size() - 1);
                } else if (focused < 0 && !model.activeSkin.images.empty()) {
                    studioImageIndex = std::min(static_cast<std::size_t>(-focused - 1),
                                                model.activeSkin.images.size() - 1);
                }
                seenElementFocusRevision = model.skinElementFocusRevision;
            }
            if (studioOpen && !previewPending && model.revision != previousRevision) {
                studioDraft = model.activeSkin;
                if (!studioDraft.images.empty()) {
                    studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
                }
            }
            target->SetTransform(D2D1::Matrix3x2F::Translation(0.0F, kTitlebarHeight));
            DrawSkinStudio(canvasSize, brushes);
            target->SetTransform(D2D1::Matrix3x2F::Identity());
            Win32Ui::Impl::DrawTitlebar(size, brushes);
        } else if (windowKind == WindowKind::YoutubeChooser) {
            target->SetTransform(D2D1::Matrix3x2F::Translation(0.0F, kTitlebarHeight));
            DrawYoutubeChooser(canvasSize, brushes);
            target->SetTransform(D2D1::Matrix3x2F::Identity());
            Win32Ui::Impl::DrawTitlebar(size, brushes);
        } else {
            if (model.skinElementFocusRevision != seenElementFocusRevision) {
                const int focused = model.focusedSkinElement;
                studioShapeFocused = focused > 0;
                studioImageFocused = focused < 0;
                if (focused > 0 && !model.activeSkin.shapes.empty()) {
                    studioShapeIndex = std::min(static_cast<std::size_t>(focused - 1),
                                                model.activeSkin.shapes.size() - 1);
                } else if (focused < 0 && !model.activeSkin.images.empty()) {
                    studioImageIndex = std::min(static_cast<std::size_t>(-focused - 1),
                                                model.activeSkin.images.size() - 1);
                }
                seenElementFocusRevision = model.skinElementFocusRevision;
            }
            if (previewFullscreen && previewIsVideo && IsVideoPreviewModuleVisible()) {
                captionRect = {};
                titlebarControlBounds.clear();
                Win32Ui::Impl::DrawPreviewFullscreenOverlay(size, brushes);
            } else {
                previewFullscreen = false;
                // The main modules remain usable at the reduced 320x200 window size.
                // Only explicit mini-player mode switches to the compact renderer.
                const bool compact = model.miniPlayer;
                target->SetTransform(D2D1::Matrix3x2F::Translation(0.0F, kTitlebarHeight));
                if (compact) DrawMini(canvasSize, brushes);
                else Win32Ui::Impl::DrawFull(canvasSize, brushes);
                target->SetTransform(D2D1::Matrix3x2F::Identity());
                Win32Ui::Impl::DrawTitlebar(size, brushes);
            }
        }
        const HRESULT result = target->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) DiscardTarget();
        EndPaint(window, &paint);
        Win32Ui::Impl::SyncRefreshTimer();
    }

void Win32Ui::Impl::Resize(UINT width, UINT height, bool minimized) {
        if (minimized || width == 0 || height == 0 || IsIconic(window)) {
            if (target && (width == 0 || height == 0)) DiscardTarget();
            if (internalModuleResize) internalModuleResize = false;
            SyncRefreshTimer();
            return;
        }

        const D2D1_SIZE_F previousSize = lastCanvas;
        const D2D1_SIZE_F newSize{static_cast<float>(width),
                                  std::max(1.0F, static_cast<float>(height) -
                                                        (HasTitlebar() ? kTitlebarHeight : 0.0F))};
        if (target && width != 0 && height != 0 &&
            FAILED(target->Resize(D2D1::SizeU(width, height)))) DiscardTarget();
        if (windowKind == WindowKind::Main && !model.miniPlayer && !internalModuleResize &&
            moduleGesture == ModuleGesture::None) {
            const bool resizeRight = windowResizeActive && windowResizeRight;
            const bool resizeBottom = windowResizeActive && windowResizeBottom;
            const bool resizeLeft = windowResizeActive && windowResizeLeft;
            const bool resizeTop = windowResizeActive && windowResizeTop;
            const bool layoutChanged = model.windowResizeBehavior == WindowResizeBehavior::ScaleAll
                ? model.moduleLayout.PreserveCollapsedExpandedGeometry(
                    previousSize.width, previousSize.height, newSize.width, newSize.height,
                    resizeRight, resizeBottom, resizeLeft, resizeTop)
                : (model.moduleLayout.PreservePixelGeometry(
                       previousSize.width, previousSize.height, newSize.width, newSize.height,
                       resizeRight, resizeBottom, resizeLeft, resizeTop) ||
                   model.moduleLayout.PreserveCollapsedExpandedGeometry(
                       previousSize.width, previousSize.height, newSize.width, newSize.height,
                       resizeRight, resizeBottom, resizeLeft, resizeTop));
            if (layoutChanged) {
                try { host.SetModuleLayout(model.moduleLayout); } catch (...) {}
            }
        }
        if (internalModuleResize) internalModuleResize = false;
        lastCanvas = newSize;
        InvalidateRect(window, nullptr, FALSE);
    }

void Win32Ui::Impl::InvokeSafely(Command command) {
        try { host.Invoke(command); } catch (...) {}
        InvalidateRect(window, nullptr, FALSE);
}

bool Win32Ui::Impl::RegisterYoutubeGrabberHotkey(std::uint32_t modifiers,
                                                 std::uint32_t virtualKey) noexcept {
        if (!window || !RegisterHotKey(window, kYoutubeGrabberHotkeyId,
                                       modifiers | MOD_NOREPEAT, virtualKey)) {
            return false;
        }
        youtubeGrabberHotkeyRegistered = true;
        youtubeGrabberHotkeyModifiers = modifiers;
        youtubeGrabberHotkeyVirtualKey = virtualKey;
        return true;
}

void Win32Ui::Impl::UnregisterYoutubeGrabberHotkey() noexcept {
        if (!youtubeGrabberHotkeyRegistered || !window) return;
        UnregisterHotKey(window, kYoutubeGrabberHotkeyId);
        youtubeGrabberHotkeyRegistered = false;
}

// ---- Notification-area (system tray) support ----------------------------

[[nodiscard]] NOTIFYICONDATAW Win32Ui::Impl::TrayIconData() const noexcept {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = kTrayIconId;
        return data;
    }

void Win32Ui::Impl::AddTrayIcon() {
        if (trayIconAdded || !window) return;
        NOTIFYICONDATAW data = TrayIconData();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = LoadRivanIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        lstrcpynW(data.szTip, L"Rivan", static_cast<int>(std::size(data.szTip)));
        if (Shell_NotifyIconW(NIM_ADD, &data)) trayIconAdded = true;
    }

void Win32Ui::Impl::RemoveTrayIcon() {
        if (!trayIconAdded) return;
        NOTIFYICONDATAW data = TrayIconData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayIconAdded = false;
    }

// Restores the hidden main window and drops the tray icon.
    void Win32Ui::Impl::RestoreFromTray() {
        if (window) {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        Win32Ui::Impl::RemoveTrayIcon();
    }

// Right-click tray menu: Open restores the window, Exit closes for real.
    void Win32Ui::Impl::ShowTrayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kTrayMenuOpen, L"Open Rivan");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");
        POINT cursor{};
        GetCursorPos(&cursor);
        // Required so the menu dismisses correctly when the user clicks elsewhere.
        SetForegroundWindow(window);
        const int command = static_cast<int>(TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, window, nullptr));
        DestroyMenu(menu);
        if (command == kTrayMenuOpen) {
            Win32Ui::Impl::RestoreFromTray();
        } else if (command == kTrayMenuExit) {
            Win32Ui::Impl::RemoveTrayIcon();
            DestroyWindow(window);
        }
    }

} // namespace rivan::ui
