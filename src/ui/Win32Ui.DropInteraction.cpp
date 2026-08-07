// Win32Ui.DropInteraction.cpp
// Native file-drop handling for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

[[nodiscard]] bool Win32Ui::Impl::IsImageDrop(const std::filesystem::path& path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".webp";
}

void Win32Ui::Impl::DropFiles(HDROP drop) {
    POINT dropPoint{};
    DragQueryPoint(drop, &dropPoint);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
    std::vector<std::wstring> audioPaths;
    std::vector<std::wstring> imagePaths;
    audioPaths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1U, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1U);
        path.resize(length);
        if (windowKind == WindowKind::Main && model.skinStudioVisible && IsImageDrop(path)) {
            imagePaths.push_back(std::move(path));
        }
        else audioPaths.push_back(std::move(path));
    }
    DragFinish(drop);

    // Image files dropped while the studio is open become positioned skin images at
    // the drop location; everything else is enqueued as audio as before.
    if (!imagePaths.empty()) AddDroppedImages(imagePaths, dropPoint);
    if (!audioPaths.empty()) {
        try { host.ImportDroppedFiles(audioPaths); } catch (...) {}
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::AddDroppedImages(const std::vector<std::wstring>& paths, POINT dropPoint) {
    if (windowKind == WindowKind::Main) studioDraft = model.activeSkin;
    float nx = 0.35F;
    float ny = 0.35F;
    if (lastCanvas.width > 0.0F && lastCanvas.height > 0.0F) {
        nx = std::clamp(static_cast<float>(dropPoint.x) / lastCanvas.width, 0.0F, 0.9F);
        const float contentY = HasTitlebar()
            ? static_cast<float>(dropPoint.y) - kTitlebarHeight : static_cast<float>(dropPoint.y);
        ny = std::clamp(contentY / lastCanvas.height, 0.0F, 0.9F);
    }
    bool changed = false;
    for (const auto& path : paths) {
        if (studioDraft.images.size() >= 32) break;
        std::wstring error;
        const auto relative = host.ImportSkinAsset(studioDraft.id, std::filesystem::path(path),
                                                   SkinAssetKind::BackgroundImage, error);
        if (!relative) {
            if (!error.empty()) MessageBoxW(window, error.c_str(), L"Rivan Skin Studio", MB_OK | MB_ICONWARNING);
            continue;
        }
        skin::SkinImage image;
        image.file = *relative;
        image.x = nx;
        image.y = ny;
        studioDraft.images.push_back(std::move(image));
        nx = std::clamp(nx + 0.05F, 0.0F, 0.9F);
        ny = std::clamp(ny + 0.05F, 0.0F, 0.9F);
        changed = true;
    }
    if (changed) PushPreview();
}

} // namespace rivan::ui
