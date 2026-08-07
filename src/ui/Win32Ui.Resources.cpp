// Win32Ui.Resources.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

[[nodiscard]] bool Win32Ui::Impl::CreateDeviceIndependentResources() {
        if (!d2dFactory) {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                         d2dFactory.ReleaseAndGetAddressOf()))) return false;
        }
        if (!writeFactory) {
            if (FAILED(DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf())))) return false;
        }
        if (!wicFactory) {
            // Non-fatal: skins without images still render. CoCreateInstance requires COM
            // initialized on this (window) thread, which App does at startup.
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(wicFactory.ReleaseAndGetAddressOf()));
        }
        if (!regularFormat) {
            auto create = [this](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                 ComPtr<IDWriteTextFormat>& format) {
                return writeFactory->CreateTextFormat(
                    family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    size, L"en-us", format.ReleaseAndGetAddressOf());
            };
            if (FAILED(create(L"Lucida Console", 11.0F, DWRITE_FONT_WEIGHT_NORMAL, regularFormat)) ||
                FAILED(create(L"MS Sans Serif", 9.0F, DWRITE_FONT_WEIGHT_NORMAL, smallFormat)) ||
                FAILED(create(L"Small Fonts", 8.0F, DWRITE_FONT_WEIGHT_NORMAL, tinyFormat)) ||
                 FAILED(create(L"MS Sans Serif", 11.0F, DWRITE_FONT_WEIGHT_BOLD, headingFormat)) ||
                 FAILED(create(L"Consolas", 30.0F, DWRITE_FONT_WEIGHT_BOLD, digitalFormat)) ||
                 FAILED(create(L"Segoe UI Symbol", 19.0F, DWRITE_FONT_WEIGHT_BOLD, studioIconFormat))) {
                return false;
            }
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            for (IDWriteTextFormat* format : {regularFormat.Get(), smallFormat.Get(), tinyFormat.Get(),
                                             headingFormat.Get(), digitalFormat.Get(), studioIconFormat.Get()}) {
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                format->SetTrimming(&trimming, nullptr);
            }
        }
        return true;
    }

[[nodiscard]] ComPtr<IDWriteFontCollection1> Win32Ui::Impl::CustomFontCollection(
        const std::filesystem::path& file) {
        ComPtr<IDWriteFontCollection1> collection;
        ComPtr<IDWriteFactory5> factory;
        ComPtr<IDWriteFontFile> fontFile;
        ComPtr<IDWriteFontSetBuilder1> builder;
        ComPtr<IDWriteFontSet> fontSet;
        if (file.empty() || FAILED(writeFactory.As(&factory)) ||
            FAILED(factory->CreateFontFileReference(file.c_str(), nullptr,
                                                    fontFile.ReleaseAndGetAddressOf())) ||
            FAILED(factory->CreateFontSetBuilder(builder.ReleaseAndGetAddressOf())) ||
            FAILED(builder->AddFontFile(fontFile.Get())) ||
            FAILED(builder->CreateFontSet(fontSet.ReleaseAndGetAddressOf())) ||
            FAILED(factory->CreateFontCollectionFromFontSet(
                fontSet.Get(), collection.ReleaseAndGetAddressOf()))) {
            return {};
        }
        return collection;
    }

[[nodiscard]] ComPtr<IDWriteTextFormat> Win32Ui::Impl::BuildTextFormat(
    const std::wstring& family, float size, DWRITE_FONT_WEIGHT weight,
    const std::filesystem::path& customFile) {
    ComPtr<IDWriteTextFormat> format;
    auto collection = CustomFontCollection(customFile);
    if (FAILED(writeFactory->CreateTextFormat(
            family.c_str(), collection.Get(), weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", format.ReleaseAndGetAddressOf()))) {
        return {};
    }
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTrimming(&trimming, nullptr);
    return format;
}

[[nodiscard]] std::optional<std::wstring> Win32Ui::Impl::FontFamilyFromFile(
        const std::filesystem::path& file) {
        auto collection = CustomFontCollection(file);
        if (!collection || collection->GetFontFamilyCount() == 0) return std::nullopt;
        ComPtr<IDWriteFontFamily> fontFamily;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(collection->GetFontFamily(0, fontFamily.ReleaseAndGetAddressOf())) ||
            FAILED(fontFamily->GetFamilyNames(names.ReleaseAndGetAddressOf())) ||
            names->GetCount() == 0) return std::nullopt;
        UINT32 index = 0;
        BOOL exists = FALSE;
        names->FindLocaleName(L"en-us", &index, &exists);
        if (!exists) index = 0;
        UINT32 length = 0;
        if (FAILED(names->GetStringLength(index, &length))) return std::nullopt;
        std::wstring family(length + 1, L'\0');
        if (FAILED(names->GetString(index, family.data(), length + 1))) return std::nullopt;
        family.resize(length);
        return family;
    }

// Rebuilds UI text formats from active skin typography. Custom files use a private
    // DirectWrite collection because FR_PRIVATE fonts are absent from its system collection.
    void Win32Ui::Impl::ApplySkinFonts() {
        const auto& type = model.activeSkin.typography;
        std::wstring family(type.fontFamily.begin(), type.fontFamily.end());
        if (family.empty()) family = L"Segoe UI";
        std::wstring customFile;
        if (!type.customFontFile.empty() && !model.activeSkin.directory.empty()) {
            customFile = (model.activeSkin.directory / type.customFontFile).wstring();
        }
        const std::wstring signature =
            family + L"|" + customFile + L"|" + std::to_wstring(static_cast<int>(type.baseSize * 4.0F));
        if (signature == fontSignature && regularFormat) return;
        fontSignature = signature;

        const float base = std::clamp(type.baseSize, 8.0F, 32.0F);
        const std::filesystem::path customPath(customFile);
        auto create = [this, &family, &customPath](float size, DWRITE_FONT_WEIGHT weight,
                                      ComPtr<IDWriteTextFormat>& format) {
            if (auto built = BuildTextFormat(family, size, weight, customPath)) format = std::move(built);
        };
        create(base, DWRITE_FONT_WEIGHT_NORMAL, regularFormat);
        create(base - 2.0F, DWRITE_FONT_WEIGHT_NORMAL, smallFormat);
        create(std::max(7.0F, base - 3.0F), DWRITE_FONT_WEIGHT_NORMAL, tinyFormat);
        create(base, DWRITE_FONT_WEIGHT_BOLD, headingFormat);
        // Time readout uses the skin font family but a fixed size: skin baseSize must not
        // scale the elapsed-time text, which is sized to fit the scope box (~22pt).
        create(27.0F, DWRITE_FONT_WEIGHT_BOLD, digitalFormat);
    }

[[nodiscard]] bool Win32Ui::Impl::CreateTarget() {
        if (target) return true;
        if (!CreateDeviceIndependentResources() || !window) return false;
        RECT client{};
        GetClientRect(window, &client);
        const auto size = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(1, client.right - client.left)),
            static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top)));
        return SUCCEEDED(d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(window, size),
            target.ReleaseAndGetAddressOf()));
    }

void Win32Ui::Impl::DiscardTarget() noexcept {
        imageCache.clear();
        trackCoverCache.clear();
        trackCoverUseCounter = 0;
        nextTrackCoverLookup = {};
        previewBitmap.Reset();
        // D2D bitmaps are device-dependent. Force latest decoded frame to re-upload.
        uploadedPreviewFrameVersion = 0;
        for (auto& brush : solidBrushes) brush.Reset();
        decorBrush.Reset();
        visualizationRenderer.DiscardDeviceResources();
        target.Reset();
    }

// Brushes are derived from the active skin palette so appearance is never hard-coded.
    // Index legend (kept for the existing draw call sites):
    //  0 windowBg 1 panelBg 2 controlBg 3 bevelLight 4 bevelDark 5 screen 6 accent
    //  7 hoverBg 8 accent 9 textPrimary 10 textSecondary 11 selection 12 textPrimary
    //  13 controls (seek/vol fill, titlebars, window chrome text, transport labels, EQ ON/AUTO)
    // Reuses solidBrushes; returns references for Draw* call sites that expect ComPtr array.
[[nodiscard]] std::array<ComPtr<ID2D1SolidColorBrush>, 14>& Win32Ui::Impl::UpdateBrushes() {
        const auto& c = model.activeSkin.colors;
        const std::array<D2D1_COLOR_F, 14> colors{
            ToD2D(c.windowBackground),
            ToD2D(c.panelBackground),
            ToD2D(c.raisedBackground),
            ToD2D(c.border),
            ToD2D(Darken(c.border, 0.45F)),
            ToD2D(c.screenBackground),
            ToD2D(c.accent),
            ToD2D(c.hoverBackground),
            ToD2D(c.accent),
            ToD2D(c.textPrimary),
            ToD2D(c.textSecondary),
            ToD2D(c.selection),
            ToD2D(c.textPrimary),
            ToD2D(c.playbackProgress),
        };
        for (std::size_t index = 0; index < solidBrushes.size(); ++index) {
            if (!solidBrushes[index]) {
                target->CreateSolidColorBrush(colors[index], solidBrushes[index].ReleaseAndGetAddressOf());
            } else {
                solidBrushes[index]->SetColor(colors[index]);
            }
        }
        // Preserve true alpha so screen opacity reveals decor and panel content underneath.
        const float screenOpacity = std::clamp(model.activeSkin.appearance.screenOpacity, 0.0F, 1.0F);
        const auto screen = ToD2D(c.screenBackground);
        const D2D1_COLOR_F screenColor{screen.r, screen.g, screen.b, screenOpacity};
        if (solidBrushes[5]) solidBrushes[5]->SetColor(screenColor);
        for (std::size_t index = 0; index < solidBrushes.size(); ++index) {
            currentBrushes[index] = solidBrushes[index].Get();
        }
        return solidBrushes;
    }

} // namespace rivan::ui
