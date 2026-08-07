// Win32Ui.SkinStudio.Rendering.cpp
// Skin Studio rendering methods for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::StudioButton(const D2D1_RECT_F& bounds, const std::wstring& label, std::uint64_t action,
                  std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b, bool active,
                  IDWriteTextFormat* format) {
    const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
    DrawBevel(bounds, (hot || active) ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(), active);
    DrawText(label, bounds, b[9].Get(), format ? format : smallFormat.Get(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    HitRegion hit;
    hit.bounds = bounds;
    hit.kind = HitKind::Studio;
    hit.id = action;
    hits.push_back(hit);
}





// Draws one left-rail section button with an icon glyph and highlights the active
// section. The label sits under the icon so the rail stays narrow.
void Win32Ui::Impl::StudioRailButton(const D2D1_RECT_F& bounds, const wchar_t* icon, const wchar_t* label,
                      StudioSection section, std::uint64_t action,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const bool active = studioSection == section;
    const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
    if (active) target->FillRectangle(bounds, b[11].Get());
    else if (hot) target->FillRectangle(bounds, b[7].Get());
    DrawText(icon, Rect(bounds.left, bounds.top + 3, bounds.right, bounds.bottom - 17),
             active ? b[12].Get() : b[6].Get(), studioIconFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(label, Rect(bounds.left, bounds.bottom - 16, bounds.right, bounds.bottom - 2),
             active ? b[12].Get() : b[10].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    HitRegion hit;
    hit.bounds = bounds;
    hit.kind = HitKind::Studio;
    hit.id = action;
    hits.push_back(hit);
}



// --- Studio section: Colors -------------------------------------------------
// Scrollable list of every semantic color with a live swatch; the selected color
// gets a HEX input (paste-friendly) plus RGBA steppers for fine control.
template <typename LabelFn, typename RowFn>
void Win32Ui::Impl::DrawStudioColors(float left, float right, float& y, LabelFn label, RowFn row,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const auto& fields = StudioColorFields();
    studioColorIndex = std::min(studioColorIndex, fields.size() - 1);
    label(L"COLORS  (click swatch to pick)");
    // Compact list: one clickable swatch+name per color.
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto r = Rect(left, y, right, y + 18);
        y += 19;
        const bool selected = i == studioColorIndex;
        const bool hot = Contains(r, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        if (selected) target->FillRectangle(r, b[11].Get());
        else if (hot) target->FillRectangle(r, b[7].Get());
        const skin::Color c = studioDraft.colors.*(fields[i].member);
        ComPtr<ID2D1SolidColorBrush> swatch;
        if (SUCCEEDED(target->CreateSolidColorBrush(ToD2D(c), swatch.ReleaseAndGetAddressOf()))) {
            DrawBevel(Rect(r.left + 2, r.top + 2, r.left + 22, r.bottom - 2), swatch.Get(),
                      b[3].Get(), b[4].Get(), true);
        }
        DrawText(fields[i].name, Rect(r.left + 28, r.top, r.right - 62, r.bottom),
                 selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
        const std::string hex = skin::FormatColor(c);
        DrawText(std::wstring(hex.begin(), hex.end()), Rect(r.right - 60, r.top, r.right - 2, r.bottom),
                 selected ? b[12].Get() : b[10].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        HitRegion hit;
        hit.bounds = r;
        hit.kind = HitKind::Studio;
        hit.id = kSelectColorBase + i;
        hits.push_back(hit);
        hit.bounds = Rect(r.left + 2, r.top + 2, r.left + 22, r.bottom - 2);
        hit.id = kOpenColorPickerBase + i;
        hits.push_back(hit);
    }
    y += 6;
    if (studioColorPickerVisible && studioColorTarget == StudioColorTarget::Palette) {
        DrawStudioColorPicker(left, right, y, row, b,
                              Action(StudioAction::ScreenEyedropper), 90);
    } else {
        studioColorPickerBounds = {};
        studioHueBounds = {};
        // Keep eyedropper + HEX available even when SV picker is collapsed.
        {
            const auto r = row();
            StudioButton(r, pickingScreenColor ? L"CLICK SCREEN..." : L"EYEDROPPER",
                         Action(StudioAction::ScreenEyedropper), b, pickingScreenColor);
        }
        label(L"HEX");
        {
            const skin::Color c = studioDraft.colors.*(fields[studioColorIndex].member);
            const auto r = row();
            DrawBevel(r, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
            std::wstring shown;
            if (studioHexEditing) {
                shown = studioHex;
                if ((GetTickCount64() / 500ULL) % 2ULL == 0ULL) shown += L"_";
            } else {
                const std::string hex = skin::FormatColor(c);
                shown = std::wstring(hex.begin(), hex.end());
            }
            if (studioHexEditing && studioHexSelectAll) {
                target->FillRectangle(Rect(r.left + 3, r.top + 2, r.right - 3, r.bottom - 2),
                                      b[11].Get());
            }
            DrawText(shown, Rect(r.left + 5, r.top, r.right - 4, r.bottom),
                      studioHexSelectAll ? b[12].Get() : b[6].Get(),
                      regularFormat.Get());
            HitRegion hit;
            hit.bounds = r;
            hit.kind = HitKind::Studio;
            hit.id = 90;
            hits.push_back(hit);
        }
    }
}



// --- Studio section: Font ---------------------------------------------------
template <typename LabelFn, typename RowFn>
void Win32Ui::Impl::DrawStudioFont(float left, float right, float& y, LabelFn label, RowFn row,
                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    (void)left; (void)right; (void)y;
    const std::wstring family(studioDraft.typography.fontFamily.begin(),
                              studioDraft.typography.fontFamily.end());
    label(L"FONT");
    {
        const auto r = row();
        StudioButton(r, L"IMPORT...", 40, b);
    }
    {
        const auto r = row();
        const float sizeLeft = r.right - 118.0F;
        StudioButton(Rect(r.left, r.top, sizeLeft - 3, r.bottom), family.c_str(), 41, b, studioFontDropdown);
        StudioButton(Rect(sizeLeft, r.top, sizeLeft + 22, r.bottom), L"-", 42, b);
        wchar_t sizeText[16]{};
        std::swprintf(sizeText, std::size(sizeText), L"%.0f pt", studioDraft.typography.baseSize);
        DrawText(sizeText, Rect(sizeLeft + 26, r.top, r.right - 26, r.bottom), b[9].Get(),
                  smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(r.right - 22, r.top, r.right, r.bottom), L"+", 43, b);
    }
    if (studioFontDropdown) {
        if (!studioDraft.typography.customFontFile.empty()) {
            const auto r = row();
            const auto customPath = studioDraft.directory / studioDraft.typography.customFontFile;
            auto customFormat = BuildTextFormat(family, studioDraft.typography.baseSize,
                                                DWRITE_FONT_WEIGHT_NORMAL, customPath);
            StudioButton(r, (L"CUSTOM  " + studioDraft.typography.customFontFile.filename().wstring()).c_str(),
                         150, b, true, customFormat.Get());
        }
        label(L"SYSTEM FONTS");
        for (std::size_t index = 0; index < kFontChoices.size(); ++index) {
            const auto r = row();
            auto previewFormat = BuildTextFormat(kFontChoices[index], studioDraft.typography.baseSize,
                                                 DWRITE_FONT_WEIGHT_NORMAL);
            StudioButton(r, kFontChoices[index], 140 + index, b,
                         studioDraft.typography.customFontFile.empty() &&
                             family == kFontChoices[index],
                         previewFormat.Get());
        }
    }
    label(L"FONT BORDER");
    {
        const auto r = row();
        StudioButton(Rect(r.left, r.top, r.left + 22, r.bottom), L"-", 45, b);
        wchar_t border[16]{};
        std::swprintf(border, std::size(border), L"%.0f px", studioDraft.typography.borderSize);
        DrawText(border, Rect(r.left + 26, r.top, r.right - 26, r.bottom), b[9].Get(),
                 smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(r.right - 22, r.top, r.right, r.bottom), L"+", 46, b);
    }
}



// --- Studio section: Toggles ------------------------------------------------
template <typename LabelFn, typename RowFn>
void Win32Ui::Impl::DrawStudioToggles(float left, float right, float& y, LabelFn label, RowFn row,
                       std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    (void)left; (void)right; (void)y;
    label(L"APPEARANCE");
    {
        const auto r = row();
        StudioButton(Rect(r.left, r.top, r.left + Width(r) / 3 - 3, r.bottom), L"TEXT BTNS", 10, b,
                     studioDraft.appearance.transparentButtons);
        StudioButton(Rect(r.left + Width(r) / 3 + 2, r.top, r.left + 2 * Width(r) / 3 - 2, r.bottom),
                     L"TITLE BARS", 11, b, studioDraft.appearance.showTitleBars);
        StudioButton(Rect(r.left + 2 * Width(r) / 3 + 3, r.top, r.right, r.bottom), L"BORDERS", 12, b,
                       studioDraft.appearance.showPanelBorders);
    }
    {
        const auto r = row();
        StudioButton(r,
                      L"CENTERED TITLES", 13, b, studioDraft.appearance.centeredTitles);
    }
    label(L"PANEL OPACITY");
    {
        const auto r = row();
        wchar_t po[24]{};
        std::swprintf(po, std::size(po), L"%.0f%%", studioDraft.appearance.panelOpacity * 100.0F);
        StudioButton(Rect(r.left, r.top, r.left + 22, r.bottom), L"-", 72, b);
        DrawText(po, Rect(r.left + 26, r.top, r.left + 90, r.bottom), b[9].Get(),
                 smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(r.left + 94, r.top, r.left + 116, r.bottom), L"+", 73, b);
    }
    label(L"SCREEN OPACITY");
    {
        const auto r = row();
        wchar_t opacity[24]{};
        std::swprintf(opacity, std::size(opacity), L"%.0f%%",
                      studioDraft.appearance.screenOpacity * 100.0F);
        StudioButton(Rect(r.left, r.top, r.left + 22, r.bottom), L"-", 76, b);
        DrawText(opacity, Rect(r.left + 26, r.top, r.left + 90, r.bottom), b[9].Get(),
                 smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(r.left + 94, r.top, r.left + 116, r.bottom), L"+", 77, b);
    }
}



// --- Studio section: Elements -----------------------------------------------
template <typename LabelFn, typename RowFn>
void Win32Ui::Impl::DrawStudioElements(float left, float right, float& y, LabelFn label, RowFn row,
                        std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    {
        const auto r = row();
        StudioButton(Rect(r.left, r.top, r.left + Width(r) * 0.5F - 3, r.bottom),
                     L"MAIN", Action(StudioAction::ShowElementEditor), b, !studioLayersTab);
        StudioButton(Rect(r.left + Width(r) * 0.5F + 3, r.top, r.right, r.bottom),
                     L"LAYERS", Action(StudioAction::ShowLayers), b, studioLayersTab);
    }
    if (studioLayersTab) {
        studioImageListBounds = {};
        studioImageRows = 0;
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        label(L"LAYERS  (DRAG TO REORDER, 1 = TOP)");
        studioLayerBounds = Rect(left, y, right, studioPanelBounds.bottom - 66.0F);
        studioLayerRows = static_cast<std::size_t>(std::max(1.0F, std::floor(Height(studioLayerBounds) / 26.0F)));
        const std::size_t maximum = order.size() > studioLayerRows ? order.size() - studioLayerRows : 0;
        studioLayerScroll = std::min(studioLayerScroll, maximum);
        const std::size_t last = std::min(order.size(), studioLayerScroll + studioLayerRows);
        for (std::size_t position = studioLayerScroll; position < last; ++position) {
            const auto& ref = order[position];
            const auto item = Rect(left, y, right, y + 22.0F);
            y += 26.0F;
            const bool selected = ref.image
                ? studioImageFocused && studioImageIndex == ref.index
                : studioShapeFocused && studioShapeIndex == ref.index;
            if (selected) target->FillRectangle(item, b[11].Get());
            DrawText(std::to_wstring(position + 1),
                     Rect(item.left + 3, item.top, item.left + 29, item.bottom),
                     selected ? b[12].Get() : b[6].Get(), smallFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            std::wstring name;
            if (ref.image) {
                if (auto* bitmap = LoadSkinBitmap(studioDraft.images[ref.index].file)) {
                    target->DrawBitmap(bitmap,
                        Rect(item.left + 31, item.top + 2, item.left + 51, item.bottom - 2),
                        1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                name = studioDraft.images[ref.index].file.filename().wstring();
            } else {
                const auto kind = studioDraft.shapes[ref.index].kind;
                name = kind == skin::ShapeKind::Ellipse ? L"SHAPE  OVAL"
                     : kind == skin::ShapeKind::Line ? L"SHAPE  LINE" : L"SHAPE  RECTANGLE";
            }
            DrawText(name, Rect(item.left + (ref.image ? 56.0F : 32.0F), item.top,
                                item.right - 5, item.bottom),
                      selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
            AddIdHit(item, HitKind::Studio, kSelectLayerBase + position);
        }
        if (order.empty()) {
            DrawText(L"Import image or add shape to create layer.", Rect(left, y, right, y + 28),
                     b[10].Get(), smallFormat.Get());
        }
        return;
    }

    const auto drawValueRow = [&](const wchar_t* name, float value, std::uint64_t decrease,
                                  std::uint64_t increase, bool percent) {
        const auto bounds = row();
        wchar_t text[48]{};
        if (percent) std::swprintf(text, std::size(text), L"%s  %.0f%%", name, value * 100.0F);
        else std::swprintf(text, std::size(text), L"%s  %.2f", name, value);
        StudioButton(Rect(bounds.left, bounds.top, bounds.left + 24, bounds.bottom),
                     L"-", decrease, b);
        DrawText(text, Rect(bounds.left + 28, bounds.top, bounds.right - 28, bounds.bottom),
                 b[9].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        StudioButton(Rect(bounds.right - 24, bounds.top, bounds.right, bounds.bottom),
                     L"+", increase, b);
    };
    auto drawImageControls = [&]() {
        auto& image = studioDraft.images[studioImageIndex];
        drawValueRow(L"X", image.x, 110, 111, false);
        drawValueRow(L"Y", image.y, 112, 113, false);
        drawValueRow(L"SIZE", image.width, 114, 115, true);
        drawValueRow(L"OPACITY", image.opacity, 116, 117, true);
        {
            const auto recolor = row();
            const float half = Width(recolor) * 0.5F;
            const bool tintActive = studioColorPickerVisible &&
                studioColorTarget == StudioColorTarget::ImageTint;
            StudioButton(Rect(recolor.left, recolor.top, recolor.left + half - 3, recolor.bottom),
                         L"TINT COLOR", Action(StudioAction::OpenImageTint), b, tintActive);
            StudioButton(Rect(recolor.left + half + 3, recolor.top, recolor.right, recolor.bottom),
                         L"CLEAR TINT", Action(StudioAction::ClearImageTint), b,
                         image.tint.alpha == 0);
            if (image.tint.alpha > 0) {
                ComPtr<ID2D1SolidColorBrush> swatch;
                if (SUCCEEDED(target->CreateSolidColorBrush(ToD2D(image.tint),
                        swatch.ReleaseAndGetAddressOf()))) {
                    DrawBevel(Rect(recolor.right - 48, recolor.top + 3, recolor.right - 28,
                                   recolor.bottom - 3), swatch.Get(), b[3].Get(), b[4].Get(), true);
                }
            }
        }
        if (studioColorPickerVisible && studioColorTarget == StudioColorTarget::ImageTint) {
            label(L"IMAGE TINT  (accent wash)");
            DrawStudioColorPicker(left, right, y, row, b,
                                  Action(StudioAction::ElementEyedropper),
                                  Action(StudioAction::ElementHexEdit));
        }
        const auto actions = row();
        const float cell = Width(actions) / 4.0F;
        StudioButton(Rect(actions.left, actions.top, actions.left + cell - 3, actions.bottom), L"DUPLICATE", 118, b);
        StudioButton(Rect(actions.left + cell, actions.top, actions.left + 2 * cell - 3, actions.bottom), L"FLIP H", 120, b, image.flipHorizontal);
        StudioButton(Rect(actions.left + 2 * cell, actions.top, actions.left + 3 * cell - 3, actions.bottom), L"FLIP V", 121, b, image.flipVertical);
        StudioButton(Rect(actions.left + 3 * cell, actions.top, actions.right, actions.bottom), L"DELETE", 119, b);
        const auto toggles = row();
        StudioButton(Rect(toggles.left, toggles.top, toggles.left + Width(toggles) * 0.5F - 3, toggles.bottom), L"IMG OVER PANELS", Action(StudioAction::ToggleImageOverPanels), b, image.overPanels);
        StudioButton(Rect(toggles.left + Width(toggles) * 0.5F + 3, toggles.top, toggles.right, toggles.bottom), L"IMG OVER SCREENS", Action(StudioAction::ToggleImageOverScreens), b, image.overScreens);
    };
    auto drawShapeControls = [&]() {
        auto& shape = studioDraft.shapes[studioShapeIndex];
        drawValueRow(L"X", shape.x, 130, 131, false);
        drawValueRow(L"Y", shape.y, 132, 133, false);
        drawValueRow(L"SIZE", shape.width, 134, 135, true);
        drawValueRow(L"OPACITY", shape.opacity, 136, 137, true);
        {
            const auto recolor = row();
            const bool recolorActive = studioColorPickerVisible &&
                studioColorTarget == StudioColorTarget::Shape;
            StudioButton(Rect(recolor.left, recolor.top, recolor.left + 96, recolor.bottom),
                         L"RECOLOR", Action(StudioAction::OpenShapeRecolor), b, recolorActive);
            ComPtr<ID2D1SolidColorBrush> swatch;
            skin::Color swatchColor = shape.color;
            swatchColor.alpha = 255;
            if (SUCCEEDED(target->CreateSolidColorBrush(ToD2D(swatchColor),
                    swatch.ReleaseAndGetAddressOf()))) {
                DrawBevel(Rect(recolor.left + 102, recolor.top + 3, recolor.left + 122,
                               recolor.bottom - 3), swatch.Get(), b[3].Get(), b[4].Get(), true);
            }
            const std::string hex = skin::FormatColor(swatchColor);
            DrawText(std::wstring(hex.begin(), hex.end()),
                     Rect(recolor.left + 128, recolor.top, recolor.right, recolor.bottom),
                     b[10].Get(), tinyFormat.Get());
        }
        if (studioColorPickerVisible && studioColorTarget == StudioColorTarget::Shape) {
            label(L"SHAPE COLOR");
            DrawStudioColorPicker(left, right, y, row, b,
                                  Action(StudioAction::ElementEyedropper),
                                  Action(StudioAction::ElementHexEdit));
        }
        if (!shape.filled) {
            const auto border = row();
            wchar_t text[48]{};
            std::swprintf(text, std::size(text), L"BORDER SIZE  %.0f px", shape.strokeWidth);
            StudioButton(Rect(border.left, border.top, border.left + 24, border.bottom), L"-", 127, b);
            DrawText(text, Rect(border.left + 28, border.top, border.right - 28, border.bottom), b[9].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            StudioButton(Rect(border.right - 24, border.top, border.right, border.bottom), L"+", 128, b);
        }
        const auto actions = row();
        const float actionCell = Width(actions) / 4.0F;
        StudioButton(Rect(actions.left, actions.top, actions.left + actionCell - 3, actions.bottom), L"DUPLICATE", 138, b);
        StudioButton(Rect(actions.left + actionCell, actions.top, actions.left + 2 * actionCell - 3, actions.bottom), L"FLIP H", 139, b, shape.flipHorizontal);
        StudioButton(Rect(actions.left + 2 * actionCell, actions.top, actions.left + 3 * actionCell - 3, actions.bottom), L"FLIP V", 146, b, shape.flipVertical);
        StudioButton(Rect(actions.left + 3 * actionCell, actions.top, actions.right, actions.bottom), L"DELETE", 147, b);
        const auto toggles = row();
        const float toggleCell = Width(toggles) / 3.0F;
        StudioButton(Rect(toggles.left, toggles.top, toggles.left + toggleCell - 3, toggles.bottom), L"OVER SCREENS", Action(StudioAction::ToggleShapeOverScreens), b, shape.overScreens);
        StudioButton(Rect(toggles.left + toggleCell, toggles.top, toggles.left + 2 * toggleCell - 3, toggles.bottom), L"TOGGLE FILL", Action(StudioAction::ToggleShapeFill), b, shape.filled);
        StudioButton(Rect(toggles.left + 2 * toggleCell, toggles.top, toggles.right, toggles.bottom), L"OVER PANELS", Action(StudioAction::ToggleShapeOverPanels), b, shape.overPanels);
    };

    wchar_t imageTitle[32]{};
    std::swprintf(imageTitle, std::size(imageTitle), L"IMAGES (%zu / 32)", studioDraft.images.size());
    label(imageTitle);
    {
        const auto r = row();
        StudioButton(r, L"IMPORT IMAGE", Action(StudioAction::ImportImage), b);
    }
    if (studioDraft.images.empty()) {
        studioImageListBounds = {};
        studioImageRows = 0;
        DrawText(L"No images.", Rect(left, y, right, y + 20), b[10].Get(), smallFormat.Get());
        y += 22.0F;
    } else {
        studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
        studioImageRows = std::min<std::size_t>(3, studioDraft.images.size());
        const std::size_t maximum = studioDraft.images.size() - studioImageRows;
        studioImageScroll = std::min(studioImageScroll, maximum);
        studioImageListBounds = Rect(left, y, right, y + studioImageRows * 24.0F);
        const std::size_t last = std::min(studioDraft.images.size(), studioImageScroll + studioImageRows);
        for (std::size_t index = studioImageScroll; index < last; ++index) {
            const auto item = Rect(left, y, right, y + 22.0F);
            y += 24.0F;
            const bool selected = studioImageFocused && index == studioImageIndex;
            if (selected) target->FillRectangle(item, b[11].Get());
            if (auto* bitmap = LoadSkinBitmap(studioDraft.images[index].file)) {
                target->DrawBitmap(bitmap, Rect(item.left + 2, item.top + 2,
                                   item.left + 22, item.bottom - 2), 1.0F,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                DrawText(L"?", Rect(item.left + 2, item.top, item.left + 22, item.bottom),
                         b[10].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            DrawText(std::to_wstring(index + 1) + L"  " + studioDraft.images[index].file.filename().wstring(),
                     Rect(item.left + 26, item.top, item.right - 5, item.bottom),
                     selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
            AddIdHit(item, HitKind::Studio, kSelectImageBase + index);
        }
        if (studioImageFocused) drawImageControls();
    }

    wchar_t shapeTitle[32]{};
    std::swprintf(shapeTitle, std::size(shapeTitle), L"SHAPES (%zu / 64)", studioDraft.shapes.size());
    label(shapeTitle);
    {
        const auto r = row();
        const float cell = Width(r) / 3.0F;
        StudioButton(Rect(r.left, r.top, r.left + cell - 3, r.bottom), L"+RECT", Action(StudioAction::AddRectangle), b);
        StudioButton(Rect(r.left + cell, r.top, r.left + 2 * cell - 3, r.bottom), L"+OVAL", Action(StudioAction::AddEllipse), b);
        StudioButton(Rect(r.left + 2 * cell, r.top, r.right, r.bottom), L"+LINE", Action(StudioAction::AddLine), b);
    }
    if (studioDraft.shapes.empty()) {
        DrawText(L"No shapes.", Rect(left, y, right, y + 20), b[10].Get(), smallFormat.Get());
        y += 22.0F;
    } else {
        studioShapeIndex = std::min(studioShapeIndex, studioDraft.shapes.size() - 1);
        const std::size_t first = studioShapeIndex >= 1 ? studioShapeIndex - 1 : 0;
        const std::size_t last = std::min(studioDraft.shapes.size(), first + 2);
        for (std::size_t index = first; index < last; ++index) {
            const auto item = Rect(left, y, right, y + 20.0F);
            y += 22.0F;
            const bool selected = studioShapeFocused && index == studioShapeIndex;
            if (selected) target->FillRectangle(item, b[11].Get());
            const auto kind = studioDraft.shapes[index].kind;
            const wchar_t* name = kind == skin::ShapeKind::Ellipse ? L"OVAL"
                : kind == skin::ShapeKind::Line ? L"LINE" : L"RECTANGLE";
            DrawText(std::to_wstring(index + 1) + L"  " + name,
                     Rect(item.left + 5, item.top, item.right - 5, item.bottom),
                     selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
            AddIdHit(item, HitKind::Studio, kSelectShapeBase + index);
        }
        if (studioShapeFocused) drawShapeControls();
    }
}



void Win32Ui::Impl::DrawSkinStudio(const D2D1_SIZE_F size,
                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    EnsureStudioDraft();
    target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
    const auto panel = Rect(10.0F, 10.0F, size.width - 10.0F, size.height - 10.0F);
    studioPanelBounds = panel;
    auto content = DrawPanel(panel, L"SKIN STUDIO", b[1].Get(), b[2].Get(), b[3].Get(),
                             b[4].Get(), b[13].Get(), b[7].Get());
     // Left icon rail: one button per customization section.
    const float railWidth = std::clamp(Width(content) * 0.16F, 62.0F, 105.0F);
    const auto rail = Rect(content.left + 2, content.top + 30, content.left + 2 + railWidth,
                           content.bottom - 30);
    // SCREEN: Skin Studio section rail.
    DrawBevel(rail, b[5].Get(), b[3].Get(), b[4].Get(), true);
    struct RailItem { const wchar_t* icon; const wchar_t* label; StudioSection section; std::uint64_t action; };
    const std::array<RailItem, 3> items{{
        {L"\u2699", L"GENERAL", StudioSection::General, Action(StudioAction::SelectGeneral)},    // gear
        {L"\U0001FAA3", L"COLORS", StudioSection::Colors, Action(StudioAction::SelectColors)},  // bucket
        {L"\U0001F5BC", L"ELEMENTS", StudioSection::Elements, Action(StudioAction::SelectElements)},  // picture
    }};
    float railTop = rail.top + 4;
    const float itemHeight = std::clamp(Height(rail) / 3.0F, 52.0F, 86.0F);
    for (const auto& item : items) {
        StudioRailButton(Rect(rail.left + 3, railTop, rail.right - 3, railTop + itemHeight - 4),
                         item.icon, item.label, item.section, item.action, b);
        railTop += itemHeight;
    }

    // Right detail pane for the active section.
    const auto pane = Rect(rail.right + 6, content.top + 30, content.right - 3, content.bottom - 30);
    // SCREEN: Skin Studio detail pane.
    DrawBevel(pane, b[5].Get(), b[3].Get(), b[4].Get(), true);
    float y = pane.top + 8;
    const float left = pane.left + 8;
    const float right = pane.right - 8;
    auto label = [&](const std::wstring& text) {
        DrawText(text, Rect(left, y, right, y + 16), b[6].Get(), smallFormat.Get());
        y += 18;
    };
    auto row = [&]() {
        const auto r = Rect(left, y, right, y + 22);
        y += 26;
        return r;
    };
    switch (studioSection) {
    case StudioSection::Colors: DrawStudioColors(left, right, y, label, row, b); break;
    case StudioSection::General:
        DrawStudioToggles(left, right, y, label, row, b);
        DrawStudioFont(left, right, y, label, row, b);
        break;
    case StudioSection::Elements: DrawStudioElements(left, right, y, label, row, b); break;
    }

    // --- Save / Cancel ---
    const auto footer = Rect(content.left + 8, content.bottom - 26, content.right - 8, content.bottom - 4);
    if (studioNameEditing) {
        const auto nameBox = Rect(footer.left, footer.top, footer.right - 82, footer.bottom);
        // SCREEN: Skin name text field.
        DrawBevel(nameBox, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        DrawText(studioName + (((GetTickCount64() / 500ULL) % 2ULL == 0ULL) ? L"_" : L""),
                 Rect(nameBox.left + 5, nameBox.top, nameBox.right - 4, nameBox.bottom), b[9].Get(),
                 regularFormat.Get());
        StudioButton(Rect(footer.right - 78, footer.top, footer.right, footer.bottom), L"SAVE", 96, b);
    } else {
        StudioButton(Rect(footer.left, footer.top, footer.left + Width(footer) * 0.5F - 3, footer.bottom),
                     L"SAVE SKIN", 1, b);
        StudioButton(Rect(footer.left + Width(footer) * 0.5F + 3, footer.top, footer.right, footer.bottom),
                     L"CANCEL", 3, b);
    }
}



} // namespace rivan::ui
