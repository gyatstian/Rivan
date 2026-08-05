// Win32Ui.Preview.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::DrawVideoPreview(
    const D2D1_RECT_F& bounds,
    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    auto content = DrawPanel(bounds, UiModuleRegistry::Get(ModuleId::VideoPreview).Title(),
                             b[1].Get(), b[2].Get(), b[3].Get(), b[4].Get(),
                             b[13].Get(), b[7].Get(), ModuleId::VideoPreview);
    previewVideoBounds = {};
    const auto preview = Rect(content.left + 3.0F, content.top + 3.0F,
                              content.right - 3.0F, content.bottom - 3.0F);
    if (Width(preview) <= 2.0F || Height(preview) <= 2.0F) return;

    Win32Ui::Impl::DrawBevel(preview, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
    previewVideoBounds = preview;
    if (!model.filePreviewEnabled) {
        Win32Ui::Impl::DrawText(L"FILE PREVIEW DISABLED", preview, b[10].Get(),
                                smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (previewBitmap) {
        Win32Ui::Impl::DrawPreviewBitmap(previewVideoBounds);
        if (previewIsVideo) {
            Win32Ui::Impl::AddSimpleHit(previewVideoBounds, HitKind::FilePreviewFullscreen);
        }
    } else {
        const wchar_t* message = L"NOTHING PLAYING";
        if (!ActivePreviewPath().empty()) {
            message = previewIsVideo ? L"LOADING PREVIEW..." : L"NO COVER AVAILABLE";
        }
        Win32Ui::Impl::DrawText(message, preview, b[10].Get(), smallFormat.Get(),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

bool Win32Ui::Impl::IsVideoPreviewModuleVisible() const noexcept {
    if (windowKind != WindowKind::Main || model.miniPlayer || !model.filePreviewEnabled) {
        return false;
    }
    const auto* item = model.moduleLayout.Find(ModuleId::VideoPreview);
    if (!item || !item->visible || item->collapsed) return false;
    if (model.moduleLayout.IsTabbed(ModuleId::VideoPreview) &&
        model.moduleLayout.tabOrder[model.moduleLayout.ActiveTabIndex()] != ModuleId::VideoPreview) {
        return false;
    }
    return true;
}

} // namespace rivan::ui
