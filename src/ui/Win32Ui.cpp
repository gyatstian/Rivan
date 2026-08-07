// Win32Ui.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

namespace {
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* ui = reinterpret_cast<Win32Ui*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ui = static_cast<Win32Ui*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return ui && ui->WindowHandle() ? ui->HandleMessage(message, wParam, lParam)
                                     : DefWindowProcW(window, message, wParam, lParam);
}
} // namespace
Win32Ui::Win32Ui(IUiHost& host) : Win32Ui(host, WindowKind::Main) {}

Win32Ui::Win32Ui(IUiHost& host, WindowKind kind) : impl_(std::make_unique<Impl>(host, kind)) {}

Win32Ui::~Win32Ui() {
    if (impl_->window && IsWindow(impl_->window)) DestroyWindow(impl_->window);
}

bool Win32Ui::Create(HINSTANCE instance, const WindowOptions& options) {
    if (impl_->window) return true;
    impl_->instance = instance;
    impl_->options = options;
    if (!impl_->CreateDeviceIndependentResources()) return false;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadRivanIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    windowClass.hIconSm = LoadRivanIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT rectangle{0, 0, std::max(options.initialWidth, options.minimumWidth),
                   std::max(options.initialHeight, options.minimumHeight)};
    const DWORD extendedStyle = (impl_->windowKind == WindowKind::Main ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW) |
                                (options.acceptFileDrops ? WS_EX_ACCEPTFILES : 0);
    AdjustWindowRectEx(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, extendedStyle);
    impl_->window = CreateWindowExW(
        extendedStyle,
        kWindowClassName, options.title ? options.title : L"Rivan", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
        options.owner, nullptr, instance, this);
    if (!impl_->window) return false;

    SendMessageW(impl_->window, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(LoadRivanIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON))));
    SendMessageW(impl_->window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(LoadRivanIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON))));

    if (options.acceptFileDrops) DragAcceptFiles(impl_->window, TRUE);
    // Force a non-client recalculation so WM_NCCALCSIZE removes the caption immediately.
    SetWindowPos(impl_->window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // WM_TIMER otherwise commonly rounds short intervals to the system's 15.6 ms tick.
    // Pair with timeEndPeriod in WM_DESTROY after the final UI window closes.
    if (impl_->windowKind == WindowKind::Main) timeBeginPeriod(1);
    ShowWindow(impl_->window, options.initiallyVisible ? SW_SHOWNORMAL : SW_HIDE);
    impl_->SyncRefreshTimer();
    if (options.initiallyVisible) UpdateWindow(impl_->window);

    if (impl_->windowKind == WindowKind::Main) {
        WindowOptions settingsOptions;
        settingsOptions.title = L"Rivan Preferences";
        settingsOptions.initialWidth = 690;
        settingsOptions.initialHeight = 500;
        settingsOptions.minimumWidth = 420;
        settingsOptions.minimumHeight = 300;
        settingsOptions.acceptFileDrops = false;
        settingsOptions.owner = impl_->window;
        settingsOptions.initiallyVisible = false;
        impl_->settingsWindow = std::unique_ptr<Win32Ui>(new Win32Ui(impl_->host, WindowKind::Settings));
        if (!impl_->settingsWindow->Create(instance, settingsOptions)) return false;
        PositionToolWindow(impl_->settingsWindow->WindowHandle(), impl_->window, 30);

        WindowOptions studioOptions;
        studioOptions.title = L"Rivan Skin Studio";
        studioOptions.initialWidth = 490;
        studioOptions.initialHeight = 600;
        studioOptions.minimumWidth = 420;
        studioOptions.minimumHeight = 420;
        studioOptions.acceptFileDrops = false;
        studioOptions.owner = impl_->window;
        studioOptions.initiallyVisible = false;
        impl_->studioWindow = std::unique_ptr<Win32Ui>(new Win32Ui(impl_->host, WindowKind::SkinStudio));
        if (!impl_->studioWindow->Create(instance, studioOptions)) return false;
        PositionToolWindow(impl_->studioWindow->WindowHandle(), impl_->window, 70);

        WindowOptions youtubeOptions;
        youtubeOptions.title = L"Rivan YouTube Download";
        youtubeOptions.initialWidth = 760;
        youtubeOptions.initialHeight = 620;
        youtubeOptions.minimumWidth = 520;
        youtubeOptions.minimumHeight = 420;
        youtubeOptions.acceptFileDrops = false;
        youtubeOptions.owner = impl_->window;
        youtubeOptions.initiallyVisible = false;
        impl_->youtubeWindow = std::unique_ptr<Win32Ui>(
            new Win32Ui(impl_->host, WindowKind::YoutubeChooser));
        if (!impl_->youtubeWindow->Create(instance, youtubeOptions)) return false;
        PositionToolWindow(impl_->youtubeWindow->WindowHandle(), impl_->window, 110);
    }
    return true;
}

HWND Win32Ui::WindowHandle() const noexcept { return impl_->window; }

void Win32Ui::Refresh() noexcept {
    if (impl_->window && IsWindowVisible(impl_->window)) {
        InvalidateRect(impl_->window, nullptr, FALSE);
    }
    if (impl_->windowKind != WindowKind::Main) return;
    UiModel current;
    try { impl_->host.SnapshotUiModel(current); } catch (...) { return; }
    if (impl_->settingsWindow && impl_->settingsWindow->WindowHandle()) {
        ShowWindow(impl_->settingsWindow->WindowHandle(), current.settingsVisible ? SW_SHOWNORMAL : SW_HIDE);
        if (current.settingsVisible) impl_->settingsWindow->Refresh();
    }
    if (impl_->studioWindow && impl_->studioWindow->WindowHandle()) {
        ShowWindow(impl_->studioWindow->WindowHandle(), current.skinStudioVisible ? SW_SHOWNORMAL : SW_HIDE);
        if (current.skinStudioVisible) impl_->studioWindow->Refresh();
    }
    if (impl_->youtubeWindow && impl_->youtubeWindow->WindowHandle()) {
        ShowWindow(impl_->youtubeWindow->WindowHandle(),
                   current.youtubeChooserVisible ? SW_SHOWNORMAL : SW_HIDE);
        if (current.youtubeChooserVisible) impl_->youtubeWindow->Refresh();
    }
}

LRESULT Win32Ui::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCALCSIZE:
        // Returning 0 with wParam TRUE makes the client area fill the entire window,
        // removing the OS caption and borders while keeping resize/snap behavior.
        if (wParam == TRUE) return 0;
        break;
    case WM_NCHITTEST:
        return impl_->HitTestNonClient(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    case WM_PAINT:
        impl_->Paint();
        return 0;
    case WM_SIZE:
        impl_->Resize(LOWORD(lParam), HIWORD(lParam));
        impl_->SyncRefreshTimer();
        return 0;
    case WM_SHOWWINDOW:
        impl_->SyncRefreshTimer();
        return 0;
    case WM_GETMINMAXINFO: {
        // Client fills window (no non-client frame), so minimum track size is client size.
        // Never derive this from module rectangles: collapsed modules keep expanded
        // geometry for restoration, but that hidden geometry must not block window resize.
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = impl_->options.minimumWidth;
        info->ptMinTrackSize.y = impl_->options.minimumHeight +
                                 (impl_->HasTitlebar() ? static_cast<int>(kTitlebarHeight) : 0);
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(impl_->window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_LBUTTONDOWN:
        impl_->PointerDown(static_cast<float>(GET_X_LPARAM(lParam)),
                           static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_RBUTTONDOWN:
        impl_->PointerRightDown(static_cast<float>(GET_X_LPARAM(lParam)),
                                static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_LBUTTONDBLCLK: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        const float contentY = impl_->HasTitlebar() ? y - kTitlebarHeight : y;
        if (impl_->previewFullscreen && impl_->windowKind == WindowKind::Main) {
            impl_->ExitPreviewFullscreen();
            return 0;
        }
        if (impl_->windowKind == WindowKind::Main && impl_->previewIsVideo &&
            impl_->IsVideoPreviewModuleVisible() &&
            Contains(impl_->previewVideoBounds, x, contentY)) {
            impl_->EnterPreviewFullscreen();
            return 0;
        }
        impl_->PointerDown(x, y);
        return 0;
    }
    case WM_MOUSEMOVE:
        impl_->PointerMove(static_cast<float>(GET_X_LPARAM(lParam)),
                           static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        impl_->PointerUp();
        return 0;
    case WM_MOUSELEAVE:
        impl_->mouse = {-1, -1};
        impl_->titlebarMouse = {-1, -1};
        InvalidateRect(impl_->window, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(impl_->window, &point);
        const int direction = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -3 : 3;
        const float contentY = impl_->HasTitlebar()
            ? static_cast<float>(point.y) - kTitlebarHeight : static_cast<float>(point.y);
        impl_->Scroll(static_cast<float>(point.x), contentY, direction);
        return 0;
    }
    case WM_CHAR:
        impl_->Character(static_cast<wchar_t>(wParam));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        impl_->KeyDown(wParam);
        return 0;
    case WM_DROPFILES:
        impl_->DropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_TIMER:
        if (wParam == kRefreshTimer) {
            if (impl_->previewPending) impl_->PushPreview();
            if (impl_->moduleGesture == Impl::ModuleGesture::Move && impl_->moduleDragActive) {
                impl_->ResolveModuleDropPreview(static_cast<float>(impl_->mouse.x),
                                                static_cast<float>(impl_->mouse.y));
            }
            InvalidateRect(impl_->window, nullptr, FALSE);
        } else if (wParam == kYoutubeSearchDebounceTimer) {
            KillTimer(impl_->window, kYoutubeSearchDebounceTimer);
            impl_->FlushYoutubeSearchDebounce();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (impl_->pickingScreenColor) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            }
            if (const HCURSOR cursor = impl_->ModuleCursor(
                    static_cast<float>(impl_->mouse.x), static_cast<float>(impl_->mouse.y))) {
                SetCursor(cursor);
                return TRUE;
            }
            if (impl_->HitTestContent(static_cast<float>(impl_->mouse.x), static_cast<float>(impl_->mouse.y))) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        if (impl_->windowKind == WindowKind::Settings) {
            impl_->InvokeSafely(Command::ToggleSettings);
            return 0;
        }
        if (impl_->windowKind == WindowKind::SkinStudio) {
            impl_->studioOpen = false;
            impl_->InvokeSafely(Command::ToggleSkinStudio);
            return 0;
        }
        if (impl_->windowKind == WindowKind::YoutubeChooser) {
            try { impl_->host.SetYoutubeChooserVisible(false); } catch (...) {}
            return 0;
        }
        if (impl_->windowKind == WindowKind::Main) {
            // Pull the latest setting; the toggle may have changed since last paint.
            try { impl_->host.SnapshotUiModel(impl_->model); } catch (...) {}
            if (impl_->model.exitToTray) {
                ShowWindow(impl_->window, SW_HIDE);
                impl_->AddTrayIcon();
                return 0;
            }
        }
        break;
    case kTrayCallbackMessage:
        if (impl_->windowKind == WindowKind::Main) {
            switch (LOWORD(lParam)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                impl_->RestoreFromTray();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                impl_->ShowTrayMenu();
                break;
            default:
                break;
            }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(impl_->window, kRefreshTimer);
        KillTimer(impl_->window, kYoutubeSearchDebounceTimer);
        impl_->RemoveTrayIcon();
        impl_->ClearFilePreview();
        impl_->StopPreviewWorker();
        impl_->DiscardTarget();
        if (impl_->windowKind == WindowKind::Main) {
            timeEndPeriod(1);
            PostQuitMessage(0);
        }
        return 0;
    case WM_NCDESTROY: {
        HWND oldWindow = impl_->window;
        SetWindowLongPtrW(oldWindow, GWLP_USERDATA, 0);
        impl_->window = nullptr;
        return DefWindowProcW(oldWindow, message, wParam, lParam);
    }
    default:
        break;
    }
    return DefWindowProcW(impl_->window, message, wParam, lParam);
}

} // namespace rivan::ui
