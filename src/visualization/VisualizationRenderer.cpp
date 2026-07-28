// VisualizationRenderer.cpp
#include "VisualizationRenderer.h"

#include <algorithm>
#include <cmath>

namespace rivan::visualization {
namespace {

[[nodiscard]] bool HasArea(const D2D1_RECT_F& rectangle) noexcept {
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

[[nodiscard]] bool ColorEqual(const D2D1_COLOR_F& left, const D2D1_COLOR_F& right) noexcept {
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

[[nodiscard]] bool PaletteEqual(const VisualizationPalette& left,
                                const VisualizationPalette& right) noexcept {
    return ColorEqual(left.panel, right.panel) && ColorEqual(left.grid, right.grid) &&
           ColorEqual(left.waveform, right.waveform) && ColorEqual(left.spectrum, right.spectrum);
}

} // namespace

void VisualizationRenderer::DiscardDeviceResources() noexcept {
    panelBrush_.Reset();
    gridBrush_.Reset();
    waveformBrush_.Reset();
    spectrumBrush_.Reset();
    boundTarget_ = nullptr;
}

HRESULT VisualizationRenderer::EnsureBrushes(ID2D1RenderTarget& target,
                                             const VisualizationPalette& palette) noexcept {
    if (boundTarget_ == &target && panelBrush_ && gridBrush_ && waveformBrush_ && spectrumBrush_ &&
        PaletteEqual(lastPalette_, palette)) {
        return S_OK;
    }

    if (boundTarget_ != &target) {
        DiscardDeviceResources();
        boundTarget_ = &target;
    }

    HRESULT result = S_OK;
    if (!panelBrush_) {
        result = target.CreateSolidColorBrush(palette.panel, &panelBrush_);
        if (FAILED(result)) return result;
    } else {
        panelBrush_->SetColor(palette.panel);
    }
    if (!gridBrush_) {
        result = target.CreateSolidColorBrush(palette.grid, &gridBrush_);
        if (FAILED(result)) return result;
    } else {
        gridBrush_->SetColor(palette.grid);
    }
    if (!waveformBrush_) {
        result = target.CreateSolidColorBrush(palette.waveform, &waveformBrush_);
        if (FAILED(result)) return result;
    } else {
        waveformBrush_->SetColor(palette.waveform);
    }
    if (!spectrumBrush_) {
        result = target.CreateSolidColorBrush(palette.spectrum, &spectrumBrush_);
        if (FAILED(result)) return result;
    } else {
        spectrumBrush_->SetColor(palette.spectrum);
    }
    lastPalette_ = palette;
    return S_OK;
}

HRESULT VisualizationRenderer::Draw(ID2D1RenderTarget& target,
                                    const D2D1_RECT_F& bounds,
                                    const VisualizationSnapshot& snapshot,
                                    const VisualizationPalette& palette) noexcept {
    if (!HasArea(bounds)) return S_FALSE;

    const HRESULT brushResult = EnsureBrushes(target, palette);
    if (FAILED(brushResult)) return brushResult;

    // Skip when fully transparent so a parent-drawn screen fill is not double-blended.
    if (palette.panel.a > 0.0F) {
        target.FillRectangle(bounds, panelBrush_.Get());
    }
    const float width = bounds.right - bounds.left;
    const float inset = std::min(12.0F, width * 0.04F);
    const D2D1_RECT_F plot{
        bounds.left + inset, bounds.top + 8.0F,
        bounds.right - inset, bounds.bottom - 8.0F};
    if (!HasArea(plot)) return S_FALSE;

    const float split = plot.top + (plot.bottom - plot.top) * 0.48F;
    const float waveformTop = plot.top;
    const float waveformBottom = split - 5.0F;
    const float spectrumTop = split + 7.0F;
    const float spectrumBottom = plot.bottom;
    const float waveformCenter = (waveformTop + waveformBottom) * 0.5F;

    target.DrawLine({plot.left, waveformCenter}, {plot.right, waveformCenter},
                    gridBrush_.Get(), 1.0F);
    target.DrawLine({plot.left, split + 1.0F}, {plot.right, split + 1.0F},
                    gridBrush_.Get(), 1.0F);
    for (int division = 1; division < 4; ++division) {
        const float x = plot.left + (plot.right - plot.left) * static_cast<float>(division) / 4.0F;
        target.DrawLine({x, plot.top}, {x, plot.bottom}, gridBrush_.Get(), 0.5F);
    }

    if (!snapshot.waveform.empty() && waveformBottom > waveformTop) {
        const std::size_t sampleCount = snapshot.waveform.size();
        // Cap columns to pixel width (and 256 max) so dense FFT windows stay cheap.
        const std::size_t columnCount = std::max<std::size_t>(
            1U, std::min<std::size_t>(
                    {sampleCount, static_cast<std::size_t>(std::ceil(width)), std::size_t{256}}));
        const float halfHeight = (waveformBottom - waveformTop) * 0.47F;
        for (std::size_t column = 0; column < columnCount; ++column) {
            const std::size_t begin = column * sampleCount / columnCount;
            const std::size_t end = std::max(begin + 1U, (column + 1U) * sampleCount / columnCount);
            float minimum = 1.0F;
            float maximum = -1.0F;
            for (std::size_t index = begin; index < std::min(end, sampleCount); ++index) {
                minimum = std::min(minimum, snapshot.waveform[index]);
                maximum = std::max(maximum, snapshot.waveform[index]);
            }
            const float x = plot.left + (static_cast<float>(column) + 0.5F) *
                (plot.right - plot.left) / static_cast<float>(columnCount);
            const float upper = waveformCenter - std::clamp(maximum, -1.0F, 1.0F) * halfHeight;
            const float lower = waveformCenter - std::clamp(minimum, -1.0F, 1.0F) * halfHeight;
            target.DrawLine({x, upper}, {x, lower}, waveformBrush_.Get(), 1.5F);
        }
    }

    if (!snapshot.spectrum.empty() && spectrumBottom > spectrumTop) {
        const float spectrumHeight = spectrumBottom - spectrumTop;
        const std::size_t binCount = snapshot.spectrum.size();
        // Fewer bars: still readable, fewer FillRectangle calls.
        const std::size_t barCount = std::clamp<std::size_t>(
            static_cast<std::size_t>((plot.right - plot.left) / 10.0F), 8U, 48U);
        constexpr float gap = 2.0F;
        const float slotWidth = (plot.right - plot.left) / static_cast<float>(barCount);
        for (std::size_t bar = 0; bar < barCount; ++bar) {
            // Logarithmic grouping gives low musical frequencies useful screen space.
            const float low = static_cast<float>(bar) / static_cast<float>(barCount);
            const float high = static_cast<float>(bar + 1U) / static_cast<float>(barCount);
            const auto first = static_cast<std::size_t>(std::floor(
                (std::pow(static_cast<float>(binCount), low) - 1.0F)));
            const auto last = static_cast<std::size_t>(std::ceil(
                (std::pow(static_cast<float>(binCount), high) - 1.0F)));
            float magnitude = 0.0F;
            for (std::size_t bin = std::min(first, binCount - 1U);
                 bin <= std::min(std::max(last, first), binCount - 1U); ++bin) {
                magnitude = std::max(magnitude, snapshot.spectrum[bin]);
            }
            const float barHeight = std::clamp(magnitude, 0.0F, 1.0F) * spectrumHeight;
            const float left = plot.left + static_cast<float>(bar) * slotWidth + gap * 0.5F;
            const float right = plot.left + static_cast<float>(bar + 1U) * slotWidth - gap * 0.5F;
            if (right > left && barHeight > 0.5F) {
                target.FillRectangle({left, spectrumBottom - barHeight, right, spectrumBottom},
                                     spectrumBrush_.Get());
            }
        }
    }

    return S_OK;
}

} // namespace rivan::visualization
