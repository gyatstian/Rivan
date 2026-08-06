// RivanModule.cpp
// Rendering for the RIVAN player module.
#include "../ui/Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::DrawPlayer(const D2D1_RECT_F& bounds,
                     std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        auto content = DrawPanel(bounds, UiModuleRegistry::Get(ModuleId::Rivan).Title(),
                                 b[1].Get(), b[2].Get(), b[3].Get(), b[4].Get(),
                                 b[13].Get(), b[7].Get(), ModuleId::Rivan);
        const float bandTop = content.top + 3;
        const float bandBottom = std::min(content.top + 88, content.bottom - 88);
        const float bandHeight = std::max(1.0F, bandBottom - bandTop);
        // Two separate screens: a near-square scope box on the left (time + visualizer)
        // and a thin marquee strip on the right showing only the song title.
        const float scopeWidth = std::min(bandHeight * 1.25F, Width(content) * 0.42F);
        const auto scopeBox = Rect(content.left + 3, bandTop, content.left + 3 + scopeWidth, bandBottom);
        // SCREEN: Player scope (time + visualizer).
        Win32Ui::Impl::DrawBevel(scopeBox, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);

        // Clickable time readout; click toggles elapsed vs. remaining.
        const double remaining = std::max(0.0, model.durationSeconds - model.positionSeconds);
        const std::wstring timeText =
            (showRemaining ? L"-" + FormatTime(remaining) : FormatTime(model.positionSeconds));
        const auto timeRect = Rect(scopeBox.left + 6, scopeBox.top + 3, scopeBox.right - 6, scopeBox.top + 34);
        Win32Ui::Impl::DrawText(timeText, timeRect, b[6].Get(), digitalFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::AddSimpleHit(timeRect, HitKind::TimeToggle);

        visualization::VisualizationPalette palette;
        // Scope DrawBevel already painted the screen fill. Re-filling here at the same
        // opacity double-blends the lower half and makes the visualizer brighter/more opaque
        // than the elapsed-time strip above it.
        palette.panel = D2D1::ColorF(0, 0, 0, 0);
        palette.grid = ToD2D(model.activeSkin.colors.accent);
        palette.waveform = ToD2D(model.activeSkin.colors.visualizationPrimary);
        palette.spectrum = ToD2D(model.activeSkin.colors.visualizationSecondary);
        const auto scope = Rect(scopeBox.left + 6, scopeBox.top + 36, scopeBox.right - 6, scopeBox.bottom - 4);
        visualizationRenderer.Draw(*target.Get(), scope, model.visualization, palette);

        // Thin marquee strip: song title scrolling to the right, like <marquee>.
        const float stripHeight = std::min(26.0F, bandHeight);
        const auto marqueeStrip = Rect(scopeBox.right + 6, bandTop, content.right - 3,
                                       bandTop + stripHeight);
        // SCREEN: Song-title marquee.
        Win32Ui::Impl::DrawBevel(marqueeStrip, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        Win32Ui::Impl::DrawMarquee(Rect(marqueeStrip.left + 4, marqueeStrip.top + 2, marqueeStrip.right - 4,
                         marqueeStrip.bottom - 2),
                    model.nowTitle.empty() ? L"RIVAN - READY" : model.nowTitle, b[6].Get());

        const float seekTop = bandBottom + 4;
        Win32Ui::Impl::DrawText(L"POS", Rect(content.left + 4, seekTop, content.left + 30, seekTop + 17),
                 b[13].Get(), tinyFormat.Get());
        const float progress = model.durationSeconds > 0.0
            ? static_cast<float>(model.positionSeconds / model.durationSeconds) : 0.0F;
        Win32Ui::Impl::DrawSlider(Rect(content.left + 32, seekTop + 1, content.right - 4, seekTop + 16), progress,
                   HitKind::Seek, b[5].Get(), b[13].Get(), b[2].Get(), b[3].Get(), b[4].Get());

        const float volumeTop = seekTop + 20;
        Win32Ui::Impl::DrawText(L"VOL", Rect(content.left + 4, volumeTop, content.left + 31, volumeTop + 16),
                 b[13].Get(), tinyFormat.Get());
        Win32Ui::Impl::DrawSlider(Rect(content.left + 32, volumeTop, content.left + Width(content) * 0.55F,
                        volumeTop + 15), model.volume, HitKind::Volume, b[5].Get(), b[13].Get(),
                   b[2].Get(), b[3].Get(), b[4].Get());
        Win32Ui::Impl::DrawText(L"BAL", Rect(content.left + Width(content) * 0.57F, volumeTop,
                              content.left + Width(content) * 0.64F, volumeTop + 16),
                 b[13].Get(), tinyFormat.Get());
        const auto balance = Rect(content.left + Width(content) * 0.65F, volumeTop,
                                  content.right - 4, volumeTop + 15);
        decorControlBounds.push_back(balance);
        registerScreenBounds = false;
        Win32Ui::Impl::DrawBevel(Rect(balance.left, volumeTop + 5, balance.right, volumeTop + 10),
                   b[5].Get(), b[3].Get(), b[4].Get(), true);
        registerScreenBounds = true;
        Win32Ui::Impl::DrawBevel(Rect((balance.left + balance.right) * 0.5F - 5, volumeTop,
                       (balance.left + balance.right) * 0.5F + 5, volumeTop + 15),
                  b[2].Get(), b[3].Get(), b[4].Get());

        const float buttonTop = content.bottom - 27;
        const float buttonWidth = std::clamp((Width(content) - 150.0F) / 4.0F, 31.0F, 44.0F);
        float x = content.left + 3;
        Win32Ui::Impl::DrawButton(Rect(x, buttonTop, x + buttonWidth, content.bottom - 2), L"|<<", Command::Previous,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        x += buttonWidth + 3;
        Win32Ui::Impl::DrawButton(Rect(x, buttonTop, x + buttonWidth, content.bottom - 2),
                   model.playback == PlaybackState::Playing ? L"||" : L">", Command::PlayPause,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get(),
                   model.playback == PlaybackState::Playing);
        x += buttonWidth + 3;
        Win32Ui::Impl::DrawButton(Rect(x, buttonTop, x + buttonWidth, content.bottom - 2), L"[]", Command::Stop,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        x += buttonWidth + 3;
        Win32Ui::Impl::DrawButton(Rect(x, buttonTop, x + buttonWidth, content.bottom - 2), L">>|", Command::Next,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawButton(Rect(content.right - 139, buttonTop, content.right - 72, content.bottom - 2),
                   L"SHUFFLE", Command::ToggleShuffle, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(),
                   b[13].Get(), model.shuffle);
        Win32Ui::Impl::DrawButton(Rect(content.right - 68, buttonTop, content.right - 2, content.bottom - 2),
                   RepeatLabel(model.repeat), Command::CycleRepeat, b[2].Get(), b[1].Get(), b[3].Get(),
                   b[4].Get(), b[13].Get(),
                   model.repeat != RepeatMode::Off);
    }

} // namespace rivan::ui
