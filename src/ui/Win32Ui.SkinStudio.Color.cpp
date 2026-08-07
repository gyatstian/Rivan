// Win32Ui.SkinStudio.Color.cpp
// Skin Studio color selection, editing, and sampling methods for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

[[nodiscard]] const std::array<Win32Ui::Impl::ColorField, 13>& Win32Ui::Impl::StudioColorFields() {
    static const std::array<ColorField, 13> fields{{
        {L"Window background", &skin::SkinPalette::windowBackground},
        {L"Panel background", &skin::SkinPalette::panelBackground},
        {L"Control background", &skin::SkinPalette::raisedBackground},
        {L"Text primary", &skin::SkinPalette::textPrimary},
        {L"Text secondary", &skin::SkinPalette::textSecondary},
        {L"Accent", &skin::SkinPalette::accent},
        {L"Hover background", &skin::SkinPalette::hoverBackground},
        {L"Border", &skin::SkinPalette::border},
        {L"Selection", &skin::SkinPalette::selection},
        {L"Screen color", &skin::SkinPalette::screenBackground},
        {L"Controls", &skin::SkinPalette::playbackProgress},
        {L"Visualization A", &skin::SkinPalette::visualizationPrimary},
        {L"Visualization B", &skin::SkinPalette::visualizationSecondary},
    }};
    return fields;
}

void Win32Ui::Impl::SelectStudioSection(StudioSection section) {
    studioSection = section;
    studioHexEditing = false;
    studioHexSelectAll = false;
    if (section == StudioSection::Colors) {
        studioColorTarget = StudioColorTarget::Palette;
    } else if (section != StudioSection::Elements) {
        studioColorPickerVisible = false;
        studioColorTarget = StudioColorTarget::Palette;
    } else if (studioColorTarget == StudioColorTarget::Palette) {
        studioColorPickerVisible = false;
    }
    draggingStudioColor = false;
    draggingStudioHue = false;
    pickingScreenColor = false;
    eyedropperSkipUp = false;
    ReleaseCapture();
    InvalidateRect(window, nullptr, FALSE);
}

[[nodiscard]] skin::Color* Win32Ui::Impl::ActiveStudioColor() {
    switch (studioColorTarget) {
    case StudioColorTarget::Shape:
        if (studioDraft.shapes.empty()) return nullptr;
        studioShapeIndex = std::min(studioShapeIndex, studioDraft.shapes.size() - 1);
        return &studioDraft.shapes[studioShapeIndex].color;
    case StudioColorTarget::ImageTint:
        if (studioDraft.images.empty()) return nullptr;
        studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
        return &studioDraft.images[studioImageIndex].tint;
    case StudioColorTarget::Palette:
    default:
        return &(studioDraft.colors.*(StudioColorFields()[studioColorIndex].member));
    }
}

void Win32Ui::Impl::OpenElementColorPicker(StudioColorTarget colorTarget) {
    studioColorTarget = colorTarget;
    studioColorPickerVisible = true;
    studioHexEditing = false;
    studioHexSelectAll = false;
    draggingStudioColor = false;
    draggingStudioHue = false;
    if (skin::Color* color = ActiveStudioColor()) {
        if (colorTarget == StudioColorTarget::Shape) {
            color->alpha = 255;
        } else if (colorTarget == StudioColorTarget::ImageTint && color->alpha == 0) {
            // Start with a visible accent so the first pick is not a no-op wash.
            color->red = 147;
            color->green = 87;
            color->blue = 255;
            color->alpha = 160;
        }
        studioHex = ToHexW(*color);
    }
    SelectStudioSection(StudioSection::Elements);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::DrawStudioColorPicker(float left, float right, float& y,
                                           const std::function<D2D1_RECT_F()>& row,
                                           std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b,
                                           std::uint64_t eyedropperAction,
                                           std::uint64_t hexAction) {
    skin::Color* selectedPtr = ActiveStudioColor();
    if (!selectedPtr) {
        studioColorPickerBounds = {};
        studioHueBounds = {};
        return;
    }
    skin::Color& selected = *selectedPtr;
    float hue{}, saturation{}, value{};
    ColorToHsv(selected, hue, saturation, value);
    studioColorPickerBounds = Rect(left, y, right, y + 68.0F);
    studioHueBounds = Rect(left, y + 73.0F, right, y + 85.0F);

    ComPtr<ID2D1GradientStopCollection> saturationStops;
    const D2D1_GRADIENT_STOP saturationGradient[] = {
        {0.0F, D2D1::ColorF(1.0F, 1.0F, 1.0F)},
        {1.0F, ToD2D(HsvColor(hue, 1.0F, 1.0F))},
    };
    ComPtr<ID2D1LinearGradientBrush> saturationBrush;
    if (SUCCEEDED(target->CreateGradientStopCollection(saturationGradient, 2,
            saturationStops.ReleaseAndGetAddressOf()))) {
        target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                {studioColorPickerBounds.left, studioColorPickerBounds.top},
                {studioColorPickerBounds.right, studioColorPickerBounds.top}),
            saturationStops.Get(), saturationBrush.ReleaseAndGetAddressOf());
    }
    if (saturationBrush) target->FillRectangle(studioColorPickerBounds, saturationBrush.Get());

    ComPtr<ID2D1GradientStopCollection> valueStops;
    const D2D1_GRADIENT_STOP valueGradient[] = {
        {0.0F, D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F)},
        {1.0F, D2D1::ColorF(0.0F, 0.0F, 0.0F, 1.0F)},
    };
    ComPtr<ID2D1LinearGradientBrush> valueBrush;
    if (SUCCEEDED(target->CreateGradientStopCollection(valueGradient, 2,
            D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            valueStops.ReleaseAndGetAddressOf()))) {
        target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                {studioColorPickerBounds.left, studioColorPickerBounds.top},
                {studioColorPickerBounds.left, studioColorPickerBounds.bottom}),
            valueStops.Get(), valueBrush.ReleaseAndGetAddressOf());
    }
    if (valueBrush) target->FillRectangle(studioColorPickerBounds, valueBrush.Get());
    target->DrawEllipse(D2D1::Ellipse(
        {studioColorPickerBounds.left + saturation * Width(studioColorPickerBounds),
         studioColorPickerBounds.top + (1.0F - value) * Height(studioColorPickerBounds)},
        4.0F, 4.0F), b[9].Get(), 1.5F);

    constexpr std::array<skin::Color, 7> hueColors{{
        {255, 0, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255},
        {0, 0, 255}, {255, 0, 255}, {255, 0, 0}}};
    std::array<D2D1_GRADIENT_STOP, hueColors.size()> hueGradient{};
    for (std::size_t index = 0; index < hueColors.size(); ++index) {
        hueGradient[index] = {static_cast<float>(index) /
                              static_cast<float>(hueColors.size() - 1), ToD2D(hueColors[index])};
    }
    ComPtr<ID2D1GradientStopCollection> hueStops;
    ComPtr<ID2D1LinearGradientBrush> hueBrush;
    if (SUCCEEDED(target->CreateGradientStopCollection(hueGradient.data(),
            static_cast<UINT32>(hueGradient.size()), hueStops.ReleaseAndGetAddressOf()))) {
        target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                {studioHueBounds.left, studioHueBounds.top},
                {studioHueBounds.right, studioHueBounds.top}),
            hueStops.Get(), hueBrush.ReleaseAndGetAddressOf());
    }
    if (hueBrush) target->FillRectangle(studioHueBounds, hueBrush.Get());
    const float hueX = studioHueBounds.left + hue * Width(studioHueBounds);
    target->DrawRectangle(Rect(hueX - 2.0F, studioHueBounds.top - 1.0F,
                               hueX + 2.0F, studioHueBounds.bottom + 1.0F), b[9].Get(), 1.0F);
    y += 92.0F;

    {
        const auto r = row();
        StudioButton(r, pickingScreenColor ? L"CLICK SCREEN..." : L"EYEDROPPER",
                     eyedropperAction, b, pickingScreenColor);
    }
    {
        const auto r = row();
        const auto box = r;
        DrawBevel(box, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        std::wstring shown;
        if (studioHexEditing) {
            shown = studioHex;
            if ((GetTickCount64() / 500ULL) % 2ULL == 0ULL) shown += L"_";
        } else {
            const std::string hex = skin::FormatColor(selected);
            shown = std::wstring(hex.begin(), hex.end());
        }
        if (studioHexEditing && studioHexSelectAll) {
            target->FillRectangle(Rect(box.left + 3, box.top + 2, box.right - 3, box.bottom - 2),
                                  b[11].Get());
        }
        DrawText(shown, Rect(box.left + 5, box.top, box.right - 4, box.bottom),
                 studioHexSelectAll ? b[12].Get() : b[6].Get(), regularFormat.Get());
        HitRegion hit;
        hit.bounds = box;
        hit.kind = HitKind::Studio;
        hit.id = hexAction;
        hits.push_back(hit);
    }
}

void Win32Ui::Impl::StepColorChannel(int channelPair) {
    auto& color = studioDraft.colors.*(StudioColorFields()[studioColorIndex].member);
    const bool increment = (channelPair % 2) != 0;
    const int channel = channelPair / 2;  // 0=R 1=G 2=B 3=A
    auto step = [increment](std::uint8_t& value) {
        const int delta = increment ? 16 : -16;
        value = static_cast<std::uint8_t>(std::clamp(static_cast<int>(value) + delta, 0, 255));
    };
    switch (channel) {
    case 0: step(color.red); break;
    case 1: step(color.green); break;
    case 2: step(color.blue); break;
    case 3: step(color.alpha); break;
    default: break;
    }
}

[[nodiscard]] std::wstring Win32Ui::Impl::ToHexW(skin::Color color) {
    const std::string value = skin::FormatColor(color);
    return std::wstring(value.begin(), value.end());
}

// Sample the desktop pixel under the cursor (any monitor) into the active studio color.
// Keeps the previous alpha channel so only RGB is replaced (except image tint, which
// forces a visible accent alpha when previously cleared).
void Win32Ui::Impl::SampleScreenColorAtCursor() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const HDC screen = GetDC(nullptr);
    if (!screen) return;
    const COLORREF pixel = GetPixel(screen, cursor.x, cursor.y);
    ReleaseDC(nullptr, screen);
    if (pixel == CLR_INVALID) return;
    skin::Color* color = ActiveStudioColor();
    if (!color) return;
    color->red = GetRValue(pixel);
    color->green = GetGValue(pixel);
    color->blue = GetBValue(pixel);
    if (studioColorTarget == StudioColorTarget::Shape) {
        color->alpha = 255;
    } else if (studioColorTarget == StudioColorTarget::ImageTint && color->alpha == 0) {
        color->alpha = 160;
    }
    studioHex = ToHexW(*color);
    studioHexEditing = false;
    studioHexSelectAll = false;
    QueuePreview();
}

void Win32Ui::Impl::BeginScreenEyedropper() {
    if (studioColorTarget == StudioColorTarget::Palette &&
        studioSection != StudioSection::Colors) {
        SelectStudioSection(StudioSection::Colors);
    } else if (studioColorTarget != StudioColorTarget::Palette &&
               studioSection != StudioSection::Elements) {
        SelectStudioSection(StudioSection::Elements);
    }
    studioColorPickerVisible = true;
    pickingScreenColor = true;
    eyedropperSkipUp = true;
    draggingStudioColor = false;
    draggingStudioHue = false;
    studioHexEditing = false;
    studioHexSelectAll = false;
    SetCapture(window);
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CancelScreenEyedropper() {
    if (!pickingScreenColor) return;
    pickingScreenColor = false;
    eyedropperSkipUp = false;
    if (GetCapture() == window) ReleaseCapture();
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::ApplyStudioHex() {
    std::string value;
    value.reserve(studioHex.size());
    for (const wchar_t character : studioHex) value.push_back(static_cast<char>(character));
    const auto parsed = skin::ParseColor(value);
    if (!parsed) {
        MessageBoxW(window, L"Enter #RRGGBB or #RRGGBBAA.", L"Invalid HEX color",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    skin::Color* color = ActiveStudioColor();
    if (!color) return;
    *color = *parsed;
    if (studioColorTarget == StudioColorTarget::Shape) {
        color->alpha = 255;
    } else if (studioColorTarget == StudioColorTarget::ImageTint && color->alpha == 0) {
        color->alpha = 160;
    }
    studioHex = ToHexW(*color);
    studioHexEditing = false;
    studioHexSelectAll = false;
    PushPreview();
}

void Win32Ui::Impl::CopyStudioHex() {
    if (!OpenClipboard(window)) return;
    const std::size_t bytes = (studioHex.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (data != nullptr) {
        if (void* destination = GlobalLock(data)) {
            std::memcpy(destination, studioHex.c_str(), bytes);
            GlobalUnlock(data);
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, data) != nullptr) data = nullptr;
        }
        if (data != nullptr) GlobalFree(data);
    }
    CloseClipboard();
}

void Win32Ui::Impl::PasteStudioHex() {
    if (!OpenClipboard(window)) return;
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text) {
            studioHex.assign(text);
            GlobalUnlock(data);
            while (!studioHex.empty() && std::iswspace(studioHex.back())) studioHex.pop_back();
            std::size_t first = 0;
            while (first < studioHex.size() && std::iswspace(studioHex[first])) ++first;
            studioHex.erase(0, first);
            if (studioHex.size() > 9) studioHex.resize(9);
            studioHexEditing = true;
            studioHexSelectAll = false;
        }
    }
    CloseClipboard();
    InvalidateRect(window, nullptr, FALSE);
}

} // namespace rivan::ui
