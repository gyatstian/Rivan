// Win32Ui.ModuleRenderer.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::DrawFull(const D2D1_SIZE_F size,
                             std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
    Win32Ui::Impl::DrawSkinDecor(size);
    screenBounds.clear();
    panelBounds.clear();
    moduleRegions.clear();
    decorControlBounds.clear();
    deferredTextLayouts.clear();
    previewVideoBounds = {};
    lyricsContentBounds = {};
    const auto& layout = moduleGesture != ModuleGesture::None
        ? moduleLayoutDraft : model.moduleLayout;
    const auto boundsFor = [size](const ModuleLayoutItem& item) {
        return ModulePixelBounds(item, size);
    };
    const auto tabActive = [&layout](ModuleId id) {
        if (layout.IsEffectivelyCollapsed(id)) return false;
        return !layout.IsTabbed(id) || layout.tabOrder[layout.ActiveTabIndex()] == id;
    };
    const auto draw = [this, &b, &boundsFor](ModuleId id, const ModuleLayoutItem& item,
                                               const ModuleLayoutItem* displayItem = nullptr) {
        const auto bounds = boundsFor(displayItem ? *displayItem : item);
        if (Width(bounds) < 2.0F || Height(bounds) < 2.0F) return;
        switch (id) {
        case ModuleId::Rivan: DrawPlayer(bounds, b); break;
        case ModuleId::AllMusic: DrawPlaylistEditor(bounds, b); break;
        case ModuleId::GraphicEqualizer: DrawEqualizer(bounds, b); break;
        case ModuleId::RivanLibrary: DrawLibrary(bounds, b); break;
        case ModuleId::VideoPreview: DrawVideoPreview(bounds, b); break;
        case ModuleId::Lyrics: DrawLyrics(bounds, b); break;
        }
    };

    if (moduleGesture == ModuleGesture::Resize && draggingModule) {
        if (const auto* item = layout.Find(*draggingModule)) {
            target->DrawRectangle(boundsFor(*item), b[8].Get(), 2.0F);
        }
    }

    deferTexts = true;
    for (const auto& item : layout.items) {
        if (!item.visible || !tabActive(item.id)) continue;
        if (layout.IsTabbed(item.id) && layout.TabCount() > 1) {
            const auto* base = layout.Find(layout.tabOrder[0]);
            if (!base) continue;
            auto display = item;
            display.x = base->x;
            display.y = base->y;
            display.width = base->width;
            display.height = base->height;
            draw(item.id, item, &display);
        } else {
            draw(item.id, item);
        }
    }
    if (layout.TabCount() > 0 &&
        !layout.IsEffectivelyCollapsed(layout.tabOrder[0])) {
        const auto* base = layout.Find(layout.tabOrder[0]);
        if (base) {
            const auto tabBounds = boundsFor(*base);
            const auto tabCount = layout.TabCount();
            const float tabWidth = std::max(44.0F, Width(tabBounds) /
                static_cast<float>(tabCount));
            const auto tabFormat = [this](bool active) -> IDWriteTextFormat* {
                return active ? headingFormat.Get() : tinyFormat.Get();
            };
            for (std::size_t index = 0; index < tabCount; ++index) {
                const auto tab = Rect(tabBounds.left + tabWidth * static_cast<float>(index),
                                      tabBounds.top + 4.0F,
                                      std::min(tabBounds.right, tabBounds.left +
                                          tabWidth * static_cast<float>(index + 1)),
                                      tabBounds.top + 22.0F);
                const bool active = index == layout.ActiveTabIndex();
                DrawBevel(tab, active ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(), active);
                DrawText(UiModuleRegistry::Get(layout.tabOrder[index]).Title(), tab, b[13].Get(),
                         tabFormat(active), DWRITE_TEXT_ALIGNMENT_CENTER);
                AddIdHit(tab, HitKind::ModuleTab,
                         (static_cast<std::uint64_t>(static_cast<std::uint8_t>(
                              layout.tabOrder[index])) << 32U) |
                         static_cast<std::uint64_t>(index));
            }
        }
    }
    if (moduleCollapseMode == ModuleCollapseMode::None &&
        moduleWindowDropZone == ModuleWindowDropZone::None && moduleDropTarget) {
        if (const auto* targetModule = layout.Find(layout.TabRoot(*moduleDropTarget))) {
            const auto targetBounds = boundsFor(*targetModule);
            D2D1_RECT_F indication = targetBounds;
            switch (moduleDropZone) {
            case ModuleDropZone::Center: break;
            case ModuleDropZone::Left:
                indication.right = (targetBounds.left + targetBounds.right) * 0.5F;
                break;
            case ModuleDropZone::Right:
                indication.left = (targetBounds.left + targetBounds.right) * 0.5F;
                break;
            case ModuleDropZone::Top:
                indication.bottom = (targetBounds.top + targetBounds.bottom) * 0.5F;
                break;
            case ModuleDropZone::Bottom:
                indication.top = (targetBounds.top + targetBounds.bottom) * 0.5F;
                break;
            case ModuleDropZone::None:
                indication = {};
                break;
            }
            if (Width(indication) > 0.0F && Height(indication) > 0.0F) {
                target->DrawRectangle(indication, b[8].Get(), 3.0F);
            }
        }
    }
    if (moduleCollapseMode == ModuleCollapseMode::None &&
        moduleWindowDropZone != ModuleWindowDropZone::None) {
        D2D1_RECT_F indication{};
        if (moduleDropPreviewValid && draggingModule) {
            if (const auto* previewItem = moduleLayoutPreview.Find(*draggingModule)) {
                indication = ModulePixelBounds(*previewItem, size);
            }
        }
        if (Width(indication) <= 0.0F || Height(indication) <= 0.0F) {
            const auto windowBounds = ModuleWindowDropBounds(moduleWindowDropZone);
            const ModuleLayoutItem indicationItem{
                ModuleId::Rivan, windowBounds.left, windowBounds.top,
                windowBounds.right - windowBounds.left,
                windowBounds.bottom - windowBounds.top};
            indication = ModulePixelBounds(indicationItem, size);
        }
        if (Width(indication) > 0.0F && Height(indication) > 0.0F) {
            target->DrawRectangle(indication, b[8].Get(), 3.0F);
        }
    }
    if (moduleCollapseMode != ModuleCollapseMode::None && moduleDropPreviewValid &&
        draggingModule) {
        if (const auto* previewItem = moduleLayoutPreview.Find(*draggingModule)) {
            const auto indication = ModuleCollapseHandleBounds(*previewItem, size);
            if (Width(indication) > 0.0F && Height(indication) > 0.0F) {
                target->DrawRectangle(indication, b[8].Get(), 3.0F);
            }
            if (previewItem->expandedWidth > 0.0F && previewItem->expandedHeight > 0.0F) {
                const auto expanded = Rect(
                    previewItem->expandedX * size.width,
                    previewItem->expandedY * size.height,
                    (previewItem->expandedX + previewItem->expandedWidth) * size.width,
                    (previewItem->expandedY + previewItem->expandedHeight) * size.height);
                target->DrawRectangle(expanded, b[8].Get(), 3.0F);
            }
        }
    }
    for (const auto& item : layout.items) {
        if (!item.visible || !layout.IsCollapseHandleVisible(item.id)) continue;
        const auto handle = ModuleCollapseHandleBounds(item, size);
        if (Width(handle) <= 1.0F || Height(handle) <= 1.0F) continue;
        const bool hot = Contains(handle, static_cast<float>(mouse.x),
                                  static_cast<float>(mouse.y));
        DrawBevel(handle, hot ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(),
                  item.collapsed);
        DrawText(ModuleCollapseArrow(item), handle, b[9].Get(), tinyFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        AddIdHit(handle, HitKind::ModuleCollapseToggle,
                 static_cast<std::uint64_t>(static_cast<std::uint8_t>(item.id)));
    }
    Win32Ui::Impl::DrawSkinDecor(size, 1);
    Win32Ui::Impl::DrawSkinDecor(size, 2);
    Win32Ui::Impl::FlushDeferredTexts();
    Win32Ui::Impl::DrawImageSelection(size);
}

} // namespace rivan::ui
