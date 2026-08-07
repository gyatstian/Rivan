// Win32UiPreviewState.h
// File preview and video-frame worker state.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rivan::ui {

struct Win32UiPreviewState {
    bool previewFullscreen{};
    std::wstring previewPath;
    D2D1_RECT_F previewVideoBounds{};
    D2D1_RECT_F previewFullscreenCloseBounds{};
    Microsoft::WRL::ComPtr<ID2D1Bitmap> previewBitmap;
    std::jthread previewWorker;
    std::mutex previewFrameMutex;
    std::mutex previewRequestMutex;
    std::condition_variable_any previewWake;
    std::wstring requestedPreviewPath;
    std::uint64_t requestedPreviewGeneration{};
    std::vector<BYTE> pendingPreviewPixels;
    UINT pendingPreviewWidth{};
    UINT pendingPreviewHeight{};
    UINT pendingPreviewStride{};
    std::atomic<double> previewWantedSeconds{0.0};
    std::atomic<std::uint64_t> pendingPreviewFrameVersion{};
    std::atomic_bool previewWorkerFailed{false};
    std::uint64_t uploadedPreviewFrameVersion{};
    bool previewIsVideo{};
    bool previewHasPresentedFrame{};
};

} // namespace rivan::ui
