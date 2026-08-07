// Win32Ui.SkinStudio.Actions.cpp
// Skin Studio draft, preview, import, and action methods for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::EnsureStudioDraft() {
    if (studioOpen) return;
    studioDraft = model.activeSkin;
    studioOpen = true;
    studioColorIndex = 0;
    studioSection = StudioSection::General;
    studioHex.clear();
    studioHexEditing = false;
    studioHexSelectAll = false;
    studioColorPickerVisible = false;
    studioColorTarget = StudioColorTarget::Palette;
    pickingScreenColor = false;
    eyedropperSkipUp = false;
    studioImageIndex = studioDraft.images.empty() ? 0 : studioDraft.images.size() - 1;
    studioImageScroll = studioImageIndex >= 2 ? studioImageIndex - 2 : 0;
}

void Win32Ui::Impl::PushPreview() {
    try { host.PreviewSkin(studioDraft); } catch (...) {}
    model.activeSkin = studioDraft;
    lastPreviewTick = GetTickCount64();
    previewPending = false;
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::QueuePreview() {
    model.activeSkin = studioDraft;
    previewPending = true;
    if (GetTickCount64() - lastPreviewTick >= kRefreshMilliseconds) PushPreview();
    else InvalidateRect(window, nullptr, FALSE);
}

[[nodiscard]] std::filesystem::path Win32Ui::Impl::PickFile(const COMDLG_FILTERSPEC* filters,
                                                             UINT filterCount) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.ReleaseAndGetAddressOf())))) {
        return {};
    }
    dialog->SetFileTypes(filterCount, filters);
    if (FAILED(dialog->Show(window))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.ReleaseAndGetAddressOf()))) return {};
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

void Win32Ui::Impl::ImportStudioImage() {
    static const COMDLG_FILTERSPEC filters[] = {
        {L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.webp"}, {L"All files", L"*.*"}};
    const auto source = PickFile(filters, 2);
    if (source.empty()) return;
    std::wstring error;
    const auto relative = host.ImportSkinAsset(studioDraft.id, source,
                                                SkinAssetKind::BackgroundImage, error);
    if (!relative) {
        if (!error.empty()) MessageBoxW(window, error.c_str(), L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
        return;
    }
    if (studioDraft.images.size() < 32) {
        skin::SkinImage image;
        image.file = *relative;
        studioDraft.images.push_back(std::move(image));
        studioImageIndex = studioDraft.images.size() - 1;
        studioImageScroll = studioImageIndex >= 2 ? studioImageIndex - 2 : 0;
        studioImageFocused = true;
        studioShapeFocused = false;
        try { host.FocusSkinElement(-(static_cast<int>(studioImageIndex) + 1)); } catch (...) {}
    }
}

void Win32Ui::Impl::ImportStudioFont() {
    static const COMDLG_FILTERSPEC filters[] = {
        {L"Fonts", L"*.ttf;*.otf;*.ttc"}, {L"All files", L"*.*"}};
    const auto source = PickFile(filters, 2);
    if (source.empty()) return;
    std::wstring error;
    const auto relative = host.ImportSkinAsset(studioDraft.id, source, SkinAssetKind::Font, error);
    if (!relative) {
        if (!error.empty()) MessageBoxW(window, error.c_str(), L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
        return;
    }
    const auto importedFamily = FontFamilyFromFile(source);
    if (!importedFamily) {
        MessageBoxW(window, L"Unable to read imported font family.", L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
        return;
    }
    studioDraft.typography.customFontFile = *relative;
    studioDraft.typography.fontFamily = Utf8(*importedFamily);
}

void Win32Ui::Impl::HandleStudioAction(std::uint64_t action) {
    // Skin Manager actions operate on installed skins, not the studio draft.
    if (action == 100) {
        try { host.Invoke(Command::ToggleSkinStudio); } catch (...) {}
        return;
    }
    if (action == 101) {
        try { host.OpenSkinFolder(); } catch (...) {}
        return;
    }
    if (action >= 1400 && action < 1500) {
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        const std::size_t position = static_cast<std::size_t>(action - 1400);
        if (position < order.size()) {
            const auto ref = order[position];
            auto& priority = ref.image ? studioDraft.images[ref.index].priority
                                       : studioDraft.shapes[ref.index].priority;
            priority = static_cast<std::uint8_t>(std::min(99, static_cast<int>(priority) + 1));
            PushPreview();
        }
        return;
    }
    if (action >= 1300 && action < 1400) {
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        const std::size_t position = static_cast<std::size_t>(action - 1300);
        if (position < order.size()) {
            const auto ref = order[position];
            auto& priority = ref.image ? studioDraft.images[ref.index].priority
                                       : studioDraft.shapes[ref.index].priority;
            priority = static_cast<std::uint8_t>(std::max(1, static_cast<int>(priority) - 1));
            PushPreview();
        }
        return;
    }
    if (action >= kSelectLayerBase && action < kSelectLayerBase + 100) {
        auto order = DecorOrder(studioDraft);
        std::reverse(order.begin(), order.end());
        const std::size_t position = static_cast<std::size_t>(action - kSelectLayerBase);
        if (position < order.size()) {
            const auto ref = order[position];
            studioImageFocused = ref.image;
            studioShapeFocused = !ref.image;
            if (ref.image) studioImageIndex = ref.index;
            else studioShapeIndex = ref.index;
            try { host.FocusSkinElement(ref.image ? -(static_cast<int>(ref.index) + 1)
                                                  : static_cast<int>(ref.index) + 1); } catch (...) {}
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= 900 && action < 1000) {
        const std::size_t index = static_cast<std::size_t>(action - 900);
        if (managerNameEditing && index == managerSkinIndex && index < model.skins.size()) {
            std::wstring error;
            if (managerSkinName.empty() || !host.RenameSkin(model.skins[index].id, managerSkinName, error)) {
                MessageBoxW(window, error.empty() ? L"Enter a skin name." : error.c_str(),
                            L"Rivan Skin Manager", MB_OK | MB_ICONWARNING);
                return;
            }
            managerNameEditing = false;
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= 800 && action < 900) {
        const std::size_t index = static_cast<std::size_t>(action - 800);
        if (index < model.skins.size() && !model.skins[index].builtIn &&
            MessageBoxW(window, (L"Delete saved skin '" + model.skins[index].name + L"'?" ).c_str(),
                        L"Rivan Skin Manager", MB_YESNO | MB_ICONWARNING) == IDYES) {
            std::wstring error;
            if (!host.DeleteSkin(model.skins[index].id, error)) {
                MessageBoxW(window, error.empty() ? L"Unable to delete skin." : error.c_str(),
                            L"Rivan Skin Manager", MB_OK | MB_ICONWARNING);
            }
        }
        return;
    }
    if (action >= 700 && action < 800) {
        const std::size_t index = static_cast<std::size_t>(action - 700);
        if (index < model.skins.size() && !model.skins[index].builtIn) host.EditSkin(model.skins[index].id);
        return;
    }
    if (action >= 600 && action < 700) {
        const std::size_t index = static_cast<std::size_t>(action - 600);
        if (index < model.skins.size() && !model.skins[index].builtIn) {
            managerSkinIndex = index;
            managerSkinName = model.skins[index].name;
            managerNameEditing = true;
            SetFocus(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
    }
    if (action >= kSelectImageBase && action < kSelectImageBase + 100) {
        const std::size_t index = static_cast<std::size_t>(action - kSelectImageBase);
        if (index < studioDraft.images.size()) {
            studioImageIndex = index;
            studioImageFocused = true;
            studioShapeFocused = false;
            if (studioColorTarget == StudioColorTarget::Shape) {
                studioColorPickerVisible = false;
                studioColorTarget = StudioColorTarget::Palette;
            }
            try { host.FocusSkinElement(-(static_cast<int>(index) + 1)); } catch (...) {}
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= kSelectShapeBase && action < kSelectShapeBase + 100) {
        const std::size_t index = static_cast<std::size_t>(action - kSelectShapeBase);
        if (index < studioDraft.shapes.size()) {
            studioShapeIndex = index;
            studioShapeFocused = true;
            studioImageFocused = false;
            if (studioColorTarget == StudioColorTarget::ImageTint) {
                studioColorPickerVisible = false;
                studioColorTarget = StudioColorTarget::Palette;
            }
            try { host.FocusSkinElement(static_cast<int>(index) + 1); } catch (...) {}
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    const auto colorCount = StudioColorFields().size();
    if (action >= kOpenColorPickerBase && action < kOpenColorPickerBase + colorCount) {
        studioColorIndex = static_cast<std::size_t>(action - kOpenColorPickerBase);
        studioColorTarget = StudioColorTarget::Palette;
        studioColorPickerVisible = true;
        studioHexEditing = false;
        studioHexSelectAll = false;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= kSelectColorBase && action < kSelectColorBase + colorCount) {
        studioColorIndex = static_cast<std::size_t>(action - kSelectColorBase);
        studioColorTarget = StudioColorTarget::Palette;
        studioHexEditing = false;
        studioHexSelectAll = false;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (action >= 200) {
        const std::size_t index = static_cast<std::size_t>(action - 200);
        if (index < model.skins.size()) {
            try { host.ApplySkin(model.skins[index].id); } catch (...) {}
        }
        return;
    }
    switch (action) {
    case Action(StudioAction::SelectColors): SelectStudioSection(StudioSection::Colors); return;
    case Action(StudioAction::SelectGeneral): SelectStudioSection(StudioSection::General); return;
    case Action(StudioAction::SelectElements): SelectStudioSection(StudioSection::Elements); return;
    case Action(StudioAction::ShowElementEditor): studioLayersTab = false; InvalidateRect(window, nullptr, FALSE); return;
    case Action(StudioAction::ShowLayers): studioLayersTab = true; InvalidateRect(window, nullptr, FALSE); return;
    default: break;
    }
    if (action == Action(StudioAction::ScreenEyedropper) ||
        action == Action(StudioAction::ElementEyedropper)) {
        if (pickingScreenColor) CancelScreenEyedropper();
        else BeginScreenEyedropper();
        return;
    }
    if (action == Action(StudioAction::OpenShapeRecolor)) {
        OpenElementColorPicker(StudioColorTarget::Shape);
        return;
    }
    if (action == Action(StudioAction::OpenImageTint)) {
        OpenElementColorPicker(StudioColorTarget::ImageTint);
        return;
    }
    if (action == Action(StudioAction::ClearImageTint)) {
        if (!studioDraft.images.empty()) {
            studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
            studioDraft.images[studioImageIndex].tint = {0, 0, 0, 0};
            if (studioColorTarget == StudioColorTarget::ImageTint) studioColorPickerVisible = false;
            PushPreview();
        }
        return;
    }
    if (action == 90 || action == Action(StudioAction::ElementHexEdit)) {
        if (skin::Color* color = ActiveStudioColor()) {
            studioHex = ToHexW(*color);
            studioHexEditing = true;
            studioHexSelectAll = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
    }

    auto& appearance = studioDraft.appearance;
    auto& type = studioDraft.typography;
    const auto& fields = StudioColorFields();
    switch (action) {
    case 1:
        studioName.assign(studioDraft.name.begin(), studioDraft.name.end());
        studioNameEditing = true;
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case 96: {
        if (studioName.empty()) {
            MessageBoxW(window, L"Enter a skin name before saving.", L"Rivan Skin Studio",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        studioDraft.name = Utf8(studioName);
        auto toSave = studioDraft;
        if (!model.skinStudioEditExisting) {
            toSave.id.clear();
            toSave.builtIn = true;
        }
        std::wstring error;
        if (!host.SaveSkin(toSave, error)) {
            MessageBoxW(window, error.empty() ? L"Unable to save skin." : error.c_str(),
                        L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
            return;
        }
        studioOpen = false;
        studioNameEditing = false;
        try { host.Invoke(Command::ToggleSkinStudio); } catch (...) {}
        return;
    }
    case 2:
    case 3:
        if (action == 3) { try { host.CancelSkinPreview(); } catch (...) {} }
        studioOpen = false;
        try { host.Invoke(Command::ToggleSkinStudio); } catch (...) {}
        return;
    case 10: appearance.transparentButtons = !appearance.transparentButtons; break;
    case 11: appearance.showTitleBars = !appearance.showTitleBars; break;
    case 12: appearance.showPanelBorders = !appearance.showPanelBorders; break;
    case 13: appearance.centeredTitles = !appearance.centeredTitles; break;
    case 20: if (studioDraft.shapes.size() < 64) { skin::SkinShape s; s.color = {255, 255, 255, 255}; studioDraft.shapes.push_back(s); studioShapeIndex = studioDraft.shapes.size() - 1; studioShapeFocused = true; studioImageFocused = false; try { host.FocusSkinElement(static_cast<int>(studioShapeIndex) + 1); } catch (...) {} } break;
    case 21: if (studioDraft.shapes.size() < 64) { skin::SkinShape s; s.kind = skin::ShapeKind::Ellipse; s.color = {255, 255, 255, 255}; studioDraft.shapes.push_back(s); studioShapeIndex = studioDraft.shapes.size() - 1; studioShapeFocused = true; studioImageFocused = false; try { host.FocusSkinElement(static_cast<int>(studioShapeIndex) + 1); } catch (...) {} } break;
    case 22: if (studioDraft.shapes.size() < 64) { skin::SkinShape s; s.kind = skin::ShapeKind::Line; s.filled = false; s.color = {255, 255, 255, 255}; studioDraft.shapes.push_back(s); studioShapeIndex = studioDraft.shapes.size() - 1; studioShapeFocused = true; studioImageFocused = false; try { host.FocusSkinElement(static_cast<int>(studioShapeIndex) + 1); } catch (...) {} } break;
    case 23: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].filled = !studioDraft.shapes[studioShapeIndex].filled; break;
    case 30: ImportStudioImage(); break;
    case 40: ImportStudioFont(); break;
    case 41: studioFontDropdown = !studioFontDropdown; InvalidateRect(window, nullptr, FALSE); return;
    case 45: type.borderSize = std::max(0.0F, type.borderSize - 1.0F); break;
    case 46: type.borderSize = std::min(8.0F, type.borderSize + 1.0F); break;
    case 140: case 141: case 142: case 143: case 144: case 145: {
        const std::size_t choice = static_cast<std::size_t>(action - 140);
        if (choice >= kFontChoices.size()) break;
        type.fontFamily = Utf8(kFontChoices[choice]);
        type.customFontFile.clear();
        studioFontDropdown = false;
        break;
    }
    case 150: studioFontDropdown = false; break;
    case 42: type.baseSize = std::clamp(type.baseSize - 1.0F, 8.0F, 32.0F); break;
    case 43: type.baseSize = std::clamp(type.baseSize + 1.0F, 8.0F, 32.0F); break;
    case 44: type.customFontFile.clear(); break;
    case 50: studioColorIndex = (studioColorIndex + fields.size() - 1) % fields.size(); break;
    case 51: studioColorIndex = (studioColorIndex + 1) % fields.size(); break;
    case 72: appearance.panelOpacity = std::clamp(appearance.panelOpacity - 0.1F, 0.0F, 1.0F); break;
    case 73: appearance.panelOpacity = std::clamp(appearance.panelOpacity + 0.1F, 0.0F, 1.0F); break;
    case 76: appearance.screenOpacity = std::clamp(appearance.screenOpacity - 0.1F, 0.0F, 1.0F); break;
    case 77: appearance.screenOpacity = std::clamp(appearance.screenOpacity + 0.1F, 0.0F, 1.0F); break;
    case 110: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].x = std::max(-1.0F, studioDraft.images[studioImageIndex].x - 0.02F); break;
    case 111: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].x = std::min(2.0F, studioDraft.images[studioImageIndex].x + 0.02F); break;
    case 112: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].y = std::max(-1.0F, studioDraft.images[studioImageIndex].y - 0.02F); break;
    case 113: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].y = std::min(2.0F, studioDraft.images[studioImageIndex].y + 0.02F); break;
    case 114: if (!studioDraft.images.empty()) { auto& image = studioDraft.images[studioImageIndex]; const float ratio = image.height / image.width; image.width = std::max(0.02F, image.width - 0.02F); image.height = image.width * ratio; } break;
    case 115: if (!studioDraft.images.empty()) { auto& image = studioDraft.images[studioImageIndex]; const float ratio = image.height / image.width; image.width = std::min(2.0F, image.width + 0.02F); image.height = std::min(2.0F, image.width * ratio); } break;
    case 116: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].opacity = std::max(0.0F, studioDraft.images[studioImageIndex].opacity - 0.05F); break;
    case 117: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].opacity = std::min(1.0F, studioDraft.images[studioImageIndex].opacity + 0.05F); break;
    case 118:
        if (!studioDraft.images.empty() && studioDraft.images.size() < 32) {
            auto copy = studioDraft.images[studioImageIndex];
            copy.x = std::min(2.0F, copy.x + 0.03F);
            copy.y = std::min(2.0F, copy.y + 0.03F);
            studioDraft.images.insert(studioDraft.images.begin() + studioImageIndex + 1, std::move(copy));
            ++studioImageIndex;
            studioImageScroll = studioImageIndex >= 2 ? studioImageIndex - 2 : 0;
        }
        break;
    case 119:
        if (!studioDraft.images.empty()) {
            studioDraft.images.erase(studioDraft.images.begin() + studioImageIndex);
            if (!studioDraft.images.empty()) studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
            else studioImageIndex = 0;
            studioImageFocused = !studioDraft.images.empty();
        }
        break;
    case 120: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].flipHorizontal = !studioDraft.images[studioImageIndex].flipHorizontal; break;
    case 121: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].flipVertical = !studioDraft.images[studioImageIndex].flipVertical; break;
    case 122: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].overPanels = !studioDraft.images[studioImageIndex].overPanels; break;
    case 123: if (!studioDraft.images.empty()) studioDraft.images[studioImageIndex].overScreens = !studioDraft.images[studioImageIndex].overScreens; break;
    case 127: if (!studioDraft.shapes.empty() && !studioDraft.shapes[studioShapeIndex].filled) studioDraft.shapes[studioShapeIndex].strokeWidth = std::max(0.0F, studioDraft.shapes[studioShapeIndex].strokeWidth - 1.0F); break;
    case 128: if (!studioDraft.shapes.empty() && !studioDraft.shapes[studioShapeIndex].filled) studioDraft.shapes[studioShapeIndex].strokeWidth = std::min(32.0F, studioDraft.shapes[studioShapeIndex].strokeWidth + 1.0F); break;
    case 130: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].x = std::max(-1.0F, studioDraft.shapes[studioShapeIndex].x - 0.02F); break;
    case 131: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].x = std::min(2.0F, studioDraft.shapes[studioShapeIndex].x + 0.02F); break;
    case 132: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].y = std::max(-1.0F, studioDraft.shapes[studioShapeIndex].y - 0.02F); break;
    case 133: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].y = std::min(2.0F, studioDraft.shapes[studioShapeIndex].y + 0.02F); break;
    case 134: if (!studioDraft.shapes.empty()) { auto& s = studioDraft.shapes[studioShapeIndex]; s.width = std::max(0.02F, s.width - 0.02F); s.height = std::max(0.02F, s.height - 0.02F); } break;
    case 135: if (!studioDraft.shapes.empty()) { auto& s = studioDraft.shapes[studioShapeIndex]; s.width = std::min(2.0F, s.width + 0.02F); s.height = std::min(2.0F, s.height + 0.02F); } break;
    case 136: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].opacity = std::max(0.0F, studioDraft.shapes[studioShapeIndex].opacity - 0.05F); break;
    case 137: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].opacity = std::min(1.0F, studioDraft.shapes[studioShapeIndex].opacity + 0.05F); break;
    case 138: if (!studioDraft.shapes.empty() && studioDraft.shapes.size() < 64) { auto copy = studioDraft.shapes[studioShapeIndex]; copy.x = std::min(2.0F, copy.x + 0.03F); copy.y = std::min(2.0F, copy.y + 0.03F); studioDraft.shapes.insert(studioDraft.shapes.begin() + studioShapeIndex + 1, copy); ++studioShapeIndex; } break;
    case 139: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].flipHorizontal = !studioDraft.shapes[studioShapeIndex].flipHorizontal; break;
    case 146: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].flipVertical = !studioDraft.shapes[studioShapeIndex].flipVertical; break;
    case 147: if (!studioDraft.shapes.empty()) { studioDraft.shapes.erase(studioDraft.shapes.begin() + studioShapeIndex); if (!studioDraft.shapes.empty()) studioShapeIndex = std::min(studioShapeIndex, studioDraft.shapes.size() - 1); else studioShapeFocused = false; } break;
    case 148: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].overScreens = !studioDraft.shapes[studioShapeIndex].overScreens; break;
    case 149: if (!studioDraft.shapes.empty()) studioDraft.shapes[studioShapeIndex].overPanels = !studioDraft.shapes[studioShapeIndex].overPanels; break;
    default:
        if (action >= 60 && action <= 67) StepColorChannel(static_cast<int>(action - 60));
        break;
    }
    if (action >= 110 && action <= 149) {
        const int focused = studioShapeFocused && !studioDraft.shapes.empty()
            ? static_cast<int>(studioShapeIndex) + 1
            : studioImageFocused && !studioDraft.images.empty()
                ? -(static_cast<int>(studioImageIndex) + 1) : 0;
        try { host.FocusSkinElement(focused); } catch (...) {}
    }
    PushPreview();
}

} // namespace rivan::ui
