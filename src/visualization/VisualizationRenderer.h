// VisualizationRenderer.h
// Direct2D renderer for the generic waveform and spectrum snapshot.
#pragma once

#include "Visualization.h"

#include <d2d1.h>
#include <wrl/client.h>

namespace rivan::visualization {

// High-contrast dark palette. Waveform and spectrum use both distinct color and
// distinct mark shape (line/envelope versus bars), so meaning is not color-only.
struct VisualizationPalette {
    D2D1_COLOR_F panel{0.047F, 0.031F, 0.086F, 1.0F};
    D2D1_COLOR_F grid{0.25F, 0.20F, 0.34F, 1.0F};
    // Validated dark-mode categorical steps; shape also distinguishes the series.
    D2D1_COLOR_F waveform{0.224F, 0.529F, 0.898F, 1.0F}; // #3987E5
    D2D1_COLOR_F spectrum{0.835F, 0.318F, 0.506F, 1.0F}; // #D55181
};

class VisualizationRenderer final {
public:
    VisualizationRenderer() = default;

    // Cached brushes; recreate only when the render target identity changes.
    HRESULT Draw(ID2D1RenderTarget& target,
                 const D2D1_RECT_F& bounds,
                 const VisualizationSnapshot& snapshot,
                 const VisualizationPalette& palette = {}) noexcept;

    void DiscardDeviceResources() noexcept;

private:
    HRESULT EnsureBrushes(ID2D1RenderTarget& target, const VisualizationPalette& palette) noexcept;

    ID2D1RenderTarget* boundTarget_{};
    VisualizationPalette lastPalette_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> gridBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> waveformBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> spectrumBrush_;
};

} // namespace rivan::visualization
