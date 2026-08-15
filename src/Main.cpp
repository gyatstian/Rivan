// Main.cpp
// Unicode Windows entry point for Rivan. It establishes process-wide desktop behavior
// and delegates all lifetime management to App.
#include "App.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ownsCom = (comResult == S_OK);

    rivan::App app(instance);
    if (!app.Initialize()) {
        MessageBoxW(nullptr, L"Rivan could not initialize. Check that Windows Media Foundation and a default audio device are available.",
                    L"Rivan", MB_OK | MB_ICONERROR);
        if (ownsCom) CoUninitialize();
        return 1;
    }

    const int result = app.Run();
    if (ownsCom) CoUninitialize();
    return result;
}
