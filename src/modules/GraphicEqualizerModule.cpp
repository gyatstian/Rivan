// GraphicEqualizerModule.cpp
// Rendering for the GRAPHIC EQUALIZER module.
#include "../ui/Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::DrawEqualizer(const D2D1_RECT_F& bounds,
                        std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        auto content = DrawPanel(bounds, UiModuleRegistry::Get(ModuleId::GraphicEqualizer).Title(),
                                 b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get(), ModuleId::GraphicEqualizer);
        Win32Ui::Impl::DrawStaticButton(Rect(content.left + 2, content.top + 2, content.left + 31, content.top + 20),
                         L"ON", b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawStaticButton(Rect(content.left + 35, content.top + 2, content.left + 77, content.top + 20),
                         L"AUTO", b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawText(L"PREAMP", Rect(content.left + 2, content.top + 22, content.left + 54, content.top + 36),
                 b[10].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);

        constexpr std::array<const wchar_t*, 10> bands{
            L"60", L"170", L"310", L"600", L"1K", L"3K", L"6K", L"12K", L"14K", L"16K"};
        const float plotLeft = content.left + 56;
        const float slot = std::max(18.0F, Width(Rect(plotLeft, 0, content.right - 3, 0)) / 10.0F);
        const float trackTop = content.top + 5;
        const float trackBottom = content.bottom - 17;
        for (std::size_t index = 0; index < bands.size(); ++index) {
            const float center = plotLeft + slot * (static_cast<float>(index) + 0.5F);
            const auto trough = Rect(center - 2, trackTop, center + 2, trackBottom);
            decorControlBounds.push_back(Rect(center - slot * 0.5F, trackTop,
                                               center + slot * 0.5F, trackBottom));
            registerScreenBounds = false;
            Win32Ui::Impl::DrawBevel(trough, b[5].Get(), b[3].Get(), b[4].Get(), true);
            registerScreenBounds = true;
            float magnitude = 0.5F;
            if (!model.visualization.spectrum.empty()) {
                const std::size_t bin = std::min(model.visualization.spectrum.size() - 1,
                    index * model.visualization.spectrum.size() / bands.size());
                magnitude = 0.25F + std::clamp(model.visualization.spectrum[bin], 0.0F, 1.0F) * 0.6F;
            }
            const float y = trackBottom - magnitude * (trackBottom - trackTop);
            Win32Ui::Impl::DrawBevel(Rect(center - 6, y - 3, center + 6, y + 3), b[2].Get(), b[3].Get(), b[4].Get());
            Win32Ui::Impl::DrawText(bands[index], Rect(center - slot * 0.5F, content.bottom - 16,
                                        center + slot * 0.5F, content.bottom), b[8].Get(), tinyFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

} // namespace rivan::ui
