// Win32Ui.h
// Resizable Direct2D/DirectWrite Win32 presentation host.
#pragma once

#include "UiHost.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>

namespace rivan::ui {

inline constexpr UINT kAudioSignalMessage = WM_APP + 43;

struct WindowOptions {
    const wchar_t* title{L"Rivan"};
    int initialWidth{1180};
    int initialHeight{760};
    // Main window modules scale with client area; minimum is only a usable drawing
    // surface, never a module-size constraint.
    int minimumWidth{120};
    int minimumHeight{80};
    bool acceptFileDrops{true};
    HWND owner{};
    bool initiallyVisible{true};
};

class Win32Ui final {
public:
    explicit Win32Ui(IUiHost& host);
    ~Win32Ui();
    Win32Ui(const Win32Ui&) = delete;
    Win32Ui& operator=(const Win32Ui&) = delete;

    // Creates a top-level window. The owner remains responsible for the message loop.
    [[nodiscard]] bool Create(HINSTANCE instance, const WindowOptions& options = {});
    [[nodiscard]] HWND WindowHandle() const noexcept;
    void Refresh() noexcept;

    // May be used by App when it owns/forwards a pre-existing window procedure.
    [[nodiscard]] LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

private:
    enum class WindowKind { Main, Settings, SkinStudio, YoutubeChooser };
    Win32Ui(IUiHost& host, WindowKind kind);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rivan::ui
