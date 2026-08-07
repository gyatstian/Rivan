// Win32UiRenderState.h
// Direct2D presentation resources and render-time caches.
#pragma once

#include "UiHost.h"
#include "../visualization/VisualizationRenderer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace rivan::ui {

struct Win32UiRenderState {
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> regularFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> smallFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tinyFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> headingFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> digitalFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> studioIconFormat;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;

    // Persistent solid brushes; colors update in place each paint.
    std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, 14> solidBrushes;
    visualization::VisualizationRenderer visualizationRenderer;
    std::map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> imageCache;

    struct TrackCoverCacheEntry {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        std::uint64_t lastUsed{};
    };
    std::map<std::wstring, TrackCoverCacheEntry> trackCoverCache;
    std::uint64_t trackCoverUseCounter{};
    std::chrono::steady_clock::time_point nextTrackCoverLookup{};

    struct DecorRef {
        bool image{};
        std::size_t index{};
        std::uint8_t priority{};
    };

    UINT currentTimerMs{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> decorBrush;

    std::uint64_t cachedTrackRowsRevision{~std::uint64_t{0}};
    std::wstring cachedTrackRowsQuery;
    std::vector<const TrackView*> cachedTrackRows;
    struct CachedSectionRow {
        bool header{};
        std::wstring label;
        const TrackView* track{};
    };
    std::uint64_t cachedSectionRowsRevision{~std::uint64_t{0}};
    std::wstring cachedSectionRowsQuery;
    std::vector<CachedSectionRow> cachedSectionRows;
    std::uint64_t cachedPlaylistDurationRevision{~std::uint64_t{0}};
    double cachedPlaylistDuration{};
    std::wstring fontSignature;

    std::array<ID2D1Brush*, 14> currentBrushes{};
    std::vector<D2D1_RECT_F> screenBounds;
    std::vector<D2D1_RECT_F> panelBounds;
    std::vector<D2D1_RECT_F> decorControlBounds;
    bool registerScreenBounds{true};
    struct DeferredText {
        std::wstring value;
        D2D1_RECT_F bounds{};
        ID2D1Brush* brush{};
        IDWriteTextFormat* format{};
        DWRITE_TEXT_ALIGNMENT alignment{DWRITE_TEXT_ALIGNMENT_LEADING};
        DWRITE_PARAGRAPH_ALIGNMENT vertical{DWRITE_PARAGRAPH_ALIGNMENT_CENTER};
    };
    std::vector<DeferredText> deferredTexts;
    struct DeferredTextLayout {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        D2D1_POINT_2F origin{};
        ID2D1Brush* brush{};
    };
    std::vector<DeferredTextLayout> deferredTextLayouts;
    bool deferTexts{};

    std::uint64_t decorOrderRevision{~std::uint64_t{0}};
    std::vector<DecorRef> decorOrder;

    D2D1_RECT_F captionRect{};
    std::vector<D2D1_RECT_F> titlebarControlBounds;
    POINT titlebarMouse{-1, -1};
};

} // namespace rivan::ui
