// Win32Ui.UpdateNotifier.cpp
// Independent release-update notifier window.
#include "Win32UiImpl.h"

namespace rivan::ui {
namespace {

constexpr std::uint64_t kOpenLatestRelease = 1;

} // namespace

void Win32Ui::Impl::DrawUpdateNotifier(
    const D2D1_SIZE_F size, std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    hits.clear();
    screenBounds.clear();
    panelBounds.clear();
    decorControlBounds.clear();
    deferredTextLayouts.clear();
    target->FillRectangle(Rect(0.0F, 0.0F, size.width, size.height), b[0].Get());

    const auto panel = Rect(12.0F, 12.0F, size.width - 12.0F, size.height - 12.0F);
    const auto content = DrawPanel(panel, L"UPDATE AVAILABLE", b[1].Get(), b[2].Get(),
                                   b[3].Get(), b[4].Get(), b[13].Get(), b[7].Get());
    const float left = content.left + 14.0F;
    const float right = content.right - 14.0F;
    const std::wstring versions = model.updateCurrentVersion + L" -> " + model.updateLatestVersion;
    DrawText(versions, Rect(left, content.top + 17.0F, right, content.top + 54.0F),
             b[12].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);

    const auto button = Rect(left, content.bottom - 42.0F, right, content.bottom - 12.0F);
    const bool hot = Contains(button, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
    DrawBevel(button, hot ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get());
    DrawText(L"OPEN LATEST RELEASE", button, b[9].Get(), smallFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    AddIdHit(button, HitKind::UpdateNotifierAction, kOpenLatestRelease);
}

void Win32Ui::Impl::HandleUpdateNotifierAction(const std::uint64_t action) {
    if (action != kOpenLatestRelease) return;
    try {
        host.OpenUpdateRelease();
    } catch (...) {
    }
    InvalidateRect(window, nullptr, FALSE);
}

} // namespace rivan::ui
