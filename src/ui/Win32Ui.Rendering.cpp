// Win32Ui.Rendering.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {







// Pulls embedded album art from ID3v2 APIC frames when Shell thumbnails fail.

















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

void Win32Ui::Impl::DrawStudioColorPicker(float left, float right, float& y, const std::function<D2D1_RECT_F()>& row,
                               std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b,
                               std::uint64_t eyedropperAction, std::uint64_t hexAction) {
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
                      studioHexSelectAll ? b[12].Get() : b[6].Get(),
                      regularFormat.Get());
            HitRegion hit;
            hit.bounds = box;
            hit.kind = HitKind::Studio;
            hit.id = hexAction;
            hits.push_back(hit);
        }
    }

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

void Win32Ui::Impl::SyncRefreshTimer() noexcept {
        if (!window || windowKind != WindowKind::Main) return;
        if (!IsWindowVisible(window) || IsIconic(window)) {
            if (currentTimerMs != 0) KillTimer(window, kRefreshTimer);
            currentTimerMs = 0;
            return;
        }
        // Video preview needs ~30 Hz while expanded even if transport is idle.
        const UINT desired = (moduleGesture != ModuleGesture::None ||
                              model.playback == PlaybackState::Playing ||
                              (IsVideoPreviewModuleVisible() && previewIsVideo) || previewFullscreen)
            ? kRefreshPlayingMilliseconds
            : kRefreshIdleMilliseconds;
        if (desired == currentTimerMs) return;
        currentTimerMs = desired;
        SetTimer(window, kRefreshTimer, currentTimerMs, nullptr);
    }

void Win32Ui::Impl::AddHit(const D2D1_RECT_F& bounds, Command command) {
        HitRegion hit;
        hit.bounds = bounds;
        hit.kind = HitKind::Command;
        hit.command = command;
        hits.push_back(hit);
    }

void Win32Ui::Impl::AddIdHit(const D2D1_RECT_F& bounds, HitKind kind, std::uint64_t id) {
        HitRegion hit;
        hit.bounds = bounds;
        hit.kind = kind;
        hit.id = id;
        hits.push_back(hit);
    }

void Win32Ui::Impl::AddSimpleHit(const D2D1_RECT_F& bounds, HitKind kind) {
        HitRegion hit;
        hit.bounds = bounds;
        hit.kind = kind;
        hits.push_back(hit);
    }

void Win32Ui::Impl::AddSettingHit(const D2D1_RECT_F& bounds, SettingCategory category) {
        HitRegion hit;
        hit.bounds = bounds;
        hit.kind = HitKind::Setting;
        hit.category = category;
        hits.push_back(hit);
    }

[[nodiscard]] const Win32Ui::Impl::HitRegion* Win32Ui::Impl::HitTest(float x, float y) const noexcept {
        for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator) {
            if (Contains(iterator->bounds, x, y)) return &*iterator;
        }
        return nullptr;
    }

void Win32Ui::Impl::DrawText(std::wstring_view textValue, const D2D1_RECT_F& bounds,
                  ID2D1Brush* brush, IDWriteTextFormat* format,
                   DWRITE_TEXT_ALIGNMENT alignment,
                   DWRITE_PARAGRAPH_ALIGNMENT vertical) {
        if (textValue.empty() || Width(bounds) <= 0.0F || Height(bounds) <= 0.0F) return;
        if (deferTexts) {
            deferredTexts.push_back({std::wstring(textValue), bounds, brush, format, alignment, vertical});
            return;
        }
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(vertical);
        const float border = std::clamp(model.activeSkin.typography.borderSize, 0.0F, 8.0F);
        if (border > 0.0F && currentBrushes[3] != nullptr) {
            // 4 cardinal offsets: ~4x cheaper than 16-sample ring; outline still readable.
            static constexpr D2D1_POINT_2F kOutlineDirs[] = {
                {1.0F, 0.0F}, {-1.0F, 0.0F}, {0.0F, 1.0F}, {0.0F, -1.0F}};
            for (const auto& dir : kOutlineDirs) {
                const auto outline = Rect(bounds.left + dir.x * border, bounds.top + dir.y * border,
                                          bounds.right + dir.x * border, bounds.bottom + dir.y * border);
                target->DrawTextW(textValue.data(), static_cast<UINT32>(textValue.size()), format, outline,
                                  currentBrushes[3], D2D1_DRAW_TEXT_OPTIONS_CLIP,
                                  DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        target->DrawTextW(textValue.data(), static_cast<UINT32>(textValue.size()), format, bounds,
                          brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        if (windowKind == WindowKind::Main && model.skinStudioVisible) {
            constexpr std::array<int, 14> brushToColor{
                0, 1, 2, 7, 7, 9, 5, 6, 5, 3, 4, 8, 3, 10};
            for (std::size_t index = 0; index < currentBrushes.size(); ++index) {
                if (brush == currentBrushes[index] && brushToColor[index] >= 0) {
                    colorFocusRegions.push_back({bounds, static_cast<std::size_t>(brushToColor[index])});
                    break;
                }
            }
        }
    }

// Horizontally scrolling single-line text, like an HTML <marquee> moving to the
    // right. The glyphs travel from the left edge toward the right and wrap around.
    void Win32Ui::Impl::DrawMarquee(const D2D1_RECT_F& bounds, const std::wstring& textValue, ID2D1Brush* brush) {
        if (textValue.empty() || Width(bounds) <= 0.0F || Height(bounds) <= 0.0F) return;
        float textWidth = Width(bounds);
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(writeFactory->CreateTextLayout(
                textValue.data(), static_cast<UINT32>(textValue.size()), regularFormat.Get(),
                100000.0F, Height(bounds), &layout))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) textWidth = metrics.width;
        }
        // Full travel span: text enters from left of view and exits at the right.
        const float span = Width(bounds) + textWidth;
        // Use millisecond phase directly. Dividing by 25 previously moved the text in
        // 25 ms steps, visibly quantizing it even when presentation reached 60 Hz.
        constexpr float kPixelsPerMillisecond = 0.04F;
        const float phase = static_cast<float>(GetTickCount64() % 10'000'000ULL) *
                            kPixelsPerMillisecond;
        const float travel = std::fmod(phase, span);
        const float x = bounds.left - textWidth + travel;
        target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        // Draw immediately: deferred text flushes after the clip is popped, letting
        // the scrolling glyphs spill over the marquee rectangle.
        const bool wasDeferred = deferTexts;
        deferTexts = false;
        DrawText(textValue, Rect(x, bounds.top, x + textWidth, bounds.bottom), brush,
                 regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
        deferTexts = wasDeferred;
        target->PopAxisAlignedClip();
    }

void Win32Ui::Impl::FlushDeferredTexts() {
        deferTexts = false;
        auto texts = std::move(deferredTexts);
        deferredTexts.clear();
        for (const auto& text : texts) {
            DrawText(text.value, text.bounds, text.brush, text.format, text.alignment, text.vertical);
        }
    }

void Win32Ui::Impl::DrawBevel(const D2D1_RECT_F& bounds, ID2D1Brush* fill, ID2D1Brush* light,
                   ID2D1Brush* dark, bool inset, float thickness) {
        const bool screen = registerScreenBounds && fill == currentBrushes[5];
        if (screen) {
            screenBounds.push_back(bounds);
        }
        target->FillRectangle(bounds, fill);
        ID2D1Brush* topLeft = inset ? dark : light;
        ID2D1Brush* bottomRight = inset ? light : dark;
        target->DrawLine({bounds.left, bounds.bottom}, {bounds.left, bounds.top}, topLeft, thickness);
        target->DrawLine({bounds.left, bounds.top}, {bounds.right, bounds.top}, topLeft, thickness);
        target->DrawLine({bounds.right, bounds.top}, {bounds.right, bounds.bottom}, bottomRight, thickness);
        target->DrawLine({bounds.right, bounds.bottom}, {bounds.left, bounds.bottom}, bottomRight, thickness);
    }

// Panels honor appearance toggles:
    //  * panelOpacity < 1 lets skin decor (images/shapes) show through the panel fill.
    //  * showTitleBars=false drops the raised metallic bar behind titles; the title text
    //    then sits directly on the panel background.
    //  * showPanelBorders=false removes the magnetic raised frame around each panel.
    [[nodiscard]] D2D1_RECT_F Win32Ui::Impl::DrawPanel(const D2D1_RECT_F& bounds, std::wstring_view titleValue,
                                         ID2D1Brush* metal, ID2D1Brush* raised, ID2D1Brush* light,
                                          ID2D1Brush* dark, ID2D1Brush* green, ID2D1Brush* /*stripe*/,
                                          std::optional<ModuleId> module) {
         panelBounds.push_back(bounds);
         if (module) moduleRegions.push_back({*module, bounds});
        const float opacity = std::clamp(model.activeSkin.appearance.panelOpacity, 0.0F, 1.0F);
        metal->SetOpacity(opacity);
        if (model.activeSkin.appearance.showPanelBorders) {
            DrawBevel(bounds, metal, light, dark, false, 2.0F);
        } else {
            target->FillRectangle(bounds, metal);
        }
        metal->SetOpacity(1.0F);
        const auto titleBar = Rect(bounds.left + 4, bounds.top + 4, bounds.right - 4, bounds.top + 22);
         if (model.activeSkin.appearance.showTitleBars) {
             DrawBevel(titleBar, raised, light, dark);
         }
         target->FillRectangle(Rect(titleBar.left + 4, titleBar.top + 5, titleBar.left + 9,
                                    titleBar.bottom - 5), green);
         const bool centered = model.activeSkin.appearance.centeredTitles;
         const auto& moduleLayout = moduleGesture != ModuleGesture::None
                                        ? moduleLayoutDraft : model.moduleLayout;
         const bool titleIsAlreadyATab = module && moduleLayout.IsTabbed(*module);
         if (!titleIsAlreadyATab) {
             DrawText(titleValue,
                      centered ? Rect(titleBar.left + 13, titleBar.top, titleBar.right - 13, titleBar.bottom)
                               : Rect(titleBar.left + 13, titleBar.top, titleBar.right - 4, titleBar.bottom),
                      green, headingFormat.Get(), centered ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                           : DWRITE_TEXT_ALIGNMENT_LEADING);
         }
         if (module && windowKind == WindowKind::Main) {
             AddIdHit(titleBar, HitKind::ModuleTitle,
                      static_cast<std::uint64_t>(static_cast<std::uint8_t>(*module)));
         }
         return Rect(bounds.left + 5, bounds.top + 25, bounds.right - 5, bounds.bottom - 5);
     }

// Transparent buttons drop the beveled metal background and use the larger regular
    // font so they read as plain text; the label brightens on hover / active. Classic
    // beveled buttons remain available when the skin disables transparent buttons.
    void Win32Ui::Impl::DrawButton(const D2D1_RECT_F& bounds, const wchar_t* label, Command command,
                     ID2D1Brush* fill, ID2D1Brush* hotFill, ID2D1Brush* light,
                     ID2D1Brush* dark, ID2D1Brush* textBrush, bool active) {
        const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        ID2D1Brush* stateFill = active ? currentBrushes[11] : currentBrushes[7];
        if (model.activeSkin.appearance.transparentButtons) {
            if (hot || active) target->FillRectangle(bounds, stateFill);
            const float previous = textBrush->GetOpacity();
            textBrush->SetOpacity((hot || active) ? 1.0F : 0.72F);
            DrawText(label, Rect(bounds.left + 2, bounds.top, bounds.right - 2, bounds.bottom),
                     textBrush, regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            textBrush->SetOpacity(previous);
        } else {
            DrawBevel(bounds, (hot || active) ? stateFill : fill, light, dark, active);
            DrawText(label, Rect(bounds.left + 2, bounds.top + 1, bounds.right - 2, bounds.bottom - 1),
                     textBrush, tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        (void)hotFill;
        AddHit(bounds, command);
    }

void Win32Ui::Impl::DrawStaticButton(const D2D1_RECT_F& bounds, const wchar_t* label,
                          ID2D1Brush* fill, ID2D1Brush* light, ID2D1Brush* dark,
                          ID2D1Brush* textBrush) {
        if (model.activeSkin.appearance.transparentButtons) {
            DrawText(label, bounds, textBrush, regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            DrawBevel(bounds, fill, light, dark);
            DrawText(label, bounds, textBrush, tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

void Win32Ui::Impl::DrawWindowButton(const D2D1_RECT_F& bounds, const wchar_t* label, std::uint64_t action,
                          ID2D1Brush* fill, ID2D1Brush* light, ID2D1Brush* dark,
                          ID2D1Brush* textBrush) {
        const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        DrawBevel(bounds, fill, hot ? textBrush : light, dark);
        DrawText(label, bounds, textBrush, tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        AddIdHit(bounds, HitKind::WindowControl, action);
    }

void Win32Ui::Impl::DrawSlider(const D2D1_RECT_F& bounds, float value, HitKind kind,
                    ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* silver,
                    ID2D1Brush* light, ID2D1Brush* dark) {
        value = std::clamp(value, 0.0F, 1.0F);
        const float center = (bounds.top + bounds.bottom) * 0.5F;
        const auto trough = Rect(bounds.left, center - 3.0F, bounds.right, center + 3.0F);
        decorControlBounds.push_back(bounds);
        registerScreenBounds = false;
        DrawBevel(trough, screen, light, dark, true);
        registerScreenBounds = true;
        target->FillRectangle(Rect(trough.left + 2, center - 1, trough.left + 2 +
                                   std::max(0.0F, Width(trough) - 4) * value, center + 1), green);
        const float knobX = bounds.left + Width(bounds) * value;
        const auto knob = Rect(knobX - 5, bounds.top, knobX + 5, bounds.bottom);
        DrawBevel(knob, silver, light, dark);
        AddSimpleHit(bounds, kind);
    }

[[nodiscard]] bool Win32Ui::Impl::IsYoutubeBrowsingNow() {
        // Paint-time model can lag right after selecting Youtube; re-snapshot cheap fields.
        try {
            host.SnapshotUiModel(model);
        } catch (...) {
        }
        return model.youtubeBrowsing;
    }

void Win32Ui::Impl::ArmYoutubeSearchDebounce() {
        if (!window || windowKind != WindowKind::Main) return;
        if (!IsYoutubeBrowsingNow()) {
            KillTimer(window, kYoutubeSearchDebounceTimer);
            return;
        }
        KillTimer(window, kYoutubeSearchDebounceTimer);
        SetTimer(window, kYoutubeSearchDebounceTimer, kYoutubeSearchDebounceMs, nullptr);
    }

void Win32Ui::Impl::FlushYoutubeSearchDebounce() {
        if (!window) return;
        KillTimer(window, kYoutubeSearchDebounceTimer);
        if (playlistQuery.empty() || !IsYoutubeBrowsingNow()) return;
        try {
            host.SubmitYoutubeQuery(playlistQuery);
        } catch (...) {
        }
    }

void Win32Ui::Impl::DrawSearch(const D2D1_RECT_F& bounds, const std::wstring& query, SearchTarget search,
                    ID2D1Brush* screen, ID2D1Brush* light, ID2D1Brush* dark,
                    ID2D1Brush* green, ID2D1Brush* dim) {
        // SCREEN: Search text field.
        DrawBevel(bounds, screen, light, dark, true, 2.0F);
        const bool active = activeSearch == search;
        const bool selectAll = active && playlistQuerySelectAll && !query.empty();
        if (selectAll) {
            target->FillRectangle(Rect(bounds.left + 3, bounds.top + 2, bounds.right - 3,
                                       bounds.bottom - 2),
                                  currentBrushes[11]);
        }
        std::wstring shown = query;
        if (shown.empty() && !active) shown = L"Search title / artist / album...";
        if (active && !selectAll && ((GetTickCount64() / 500ULL) % 2ULL == 0ULL)) shown += L"_";
        DrawText(shown, Rect(bounds.left + 5, bounds.top, bounds.right - 4, bounds.bottom),
                 query.empty() && !active ? dim : (selectAll ? currentBrushes[12] : green),
                 regularFormat.Get());
        (void)search;
        AddSimpleHit(bounds, HitKind::PlaylistSearch);
    }

[[nodiscard]] const std::vector<const TrackView*>& Win32Ui::Impl::Filtered(const std::vector<TrackView>& source,
                                                                  const std::wstring& query) {
        if (cachedTrackRowsRevision == model.revision && cachedTrackRowsQuery == query) {
            return cachedTrackRows;
        }
        cachedTrackRowsRevision = model.revision;
        cachedTrackRowsQuery = query;
        cachedTrackRows.clear();
        cachedTrackRows.reserve(source.size());
        for (const auto& track : source) {
            if (Matches(track, query)) cachedTrackRows.push_back(&track);
        }
        return cachedTrackRows;
    }

// Position of a TrackView (borrowed from model.tracks) within that vector. Selection
    // and drag reorder key off this stable model index, not the filtered row index, so
    // duplicate entries and search filtering stay unambiguous.
[[nodiscard]] std::size_t Win32Ui::Impl::ModelTrackIndex(const TrackView* track) const noexcept {
        if (model.tracks.empty()) return static_cast<std::size_t>(-1);
        const auto* base = model.tracks.data();
        if (track < base || track >= base + model.tracks.size()) {
            return static_cast<std::size_t>(-1);
        }
        return static_cast<std::size_t>(track - base);
    }

[[nodiscard]] std::size_t Win32Ui::Impl::SourceTrackIndex(const TrackView* track) const noexcept {
        const auto modelIndex = ModelTrackIndex(track);
        if (modelIndex == static_cast<std::size_t>(-1)) return modelIndex;
        const auto source = track->sourcePlaylistId;
        if (source == 0) return modelIndex;
        std::size_t sourceIndex = 0;
        for (std::size_t index = 0; index < modelIndex; ++index) {
            if (model.tracks[index].sourcePlaylistId == source) ++sourceIndex;
        }
        return model.tracks[modelIndex].sourcePlaylistId == source
                   ? sourceIndex
                   : static_cast<std::size_t>(-1);
    }

void Win32Ui::Impl::DrawTrackRenameField(const D2D1_RECT_F& bounds, ID2D1Brush* textBrush) {
        Win32Ui::Impl::DrawBevel(bounds, currentBrushes[3], currentBrushes[4], currentBrushes[5], true);
        const auto textBounds = Rect(bounds.left + 4, bounds.top, bounds.right - 4, bounds.bottom);
        if (trackNameSelectAll) target->FillRectangle(textBounds, currentBrushes[11]);
        Win32Ui::Impl::DrawText(trackNameBuffer, textBounds, textBrush, regularFormat.Get());
        if (trackNameSelectAll) return;

        const std::size_t cursor = std::min(trackNameCursor, trackNameBuffer.size());
        float caretX = textBounds.left;
        if (writeFactory && regularFormat && !trackNameBuffer.empty()) {
            regularFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            regularFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(writeFactory->CreateTextLayout(
                    trackNameBuffer.data(), static_cast<UINT32>(trackNameBuffer.size()),
                    regularFormat.Get(), Width(textBounds), Height(textBounds), &layout))) {
                FLOAT x{};
                FLOAT y{};
                DWRITE_HIT_TEST_METRICS metrics{};
                if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(cursor), FALSE,
                                                          &x, &y, &metrics))) {
                    caretX = textBounds.left + x;
                }
            }
        }
        caretX = std::clamp(caretX, textBounds.left, textBounds.right - 1.0F);
        target->DrawLine({caretX, textBounds.top + 3.0F}, {caretX, textBounds.bottom - 3.0F},
                         textBrush, 1.0F);
    }

// Thin horizontal insertion bar drawn between rows while a track drag is active.
    void Win32Ui::Impl::DrawTrackDropIndicator(const D2D1_RECT_F& row, bool below, ID2D1Brush* brush) {
        const float y = below ? row.bottom : row.top;
        target->FillRectangle(Rect(row.left + 2, y - 1.0F, row.right - 2, y + 1.0F), brush);
    }

void Win32Ui::Impl::DrawTrackRows(const D2D1_RECT_F& bounds, const std::vector<const TrackView*>& tracks,
                       std::size_t& scroll, std::size_t& visibleRows,
                       ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* greenDim,
                       ID2D1Brush* selection, ID2D1Brush* white, ID2D1Brush* dim,
                         bool showArtist) {
        (void)dim;  // Track lengths now share the title color; kept for signature parity.
        // SCREEN: Track list, including the < NO MATCHING TRACKS > state.
        if (screen == currentBrushes[5]) screenBounds.push_back(bounds);
        target->FillRectangle(bounds, screen);
        visibleRows = static_cast<std::size_t>(std::max(0.0F, std::floor(Height(bounds) / kTrackRowHeight)));
        const std::size_t maximum = tracks.size() > visibleRows ? tracks.size() - visibleRows : 0;
        scroll = std::min(scroll, maximum);
        const bool dragging = dragActive && dragKind == DragKind::Track;
        target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        for (std::size_t rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
            const std::size_t index = scroll + rowIndex;
            if (index >= tracks.size()) break;
            const auto& track = *tracks[index];
            const std::size_t modelIndex = ModelTrackIndex(tracks[index]);
            const bool selected = trackSelection.contains(modelIndex);
            const float top = bounds.top + static_cast<float>(rowIndex) * kTrackRowHeight;
            const auto row = Rect(bounds.left, top, bounds.right, top + kTrackRowHeight);
            const bool hot = Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (selected || track.playing || hot) {
                target->FillRectangle(row, selected || track.playing ? selection
                                                                     : currentBrushes[7]);
            }
            if (track.playing) {
                Win32Ui::Impl::DrawText(L">", Rect(row.left + 2, row.top, row.left + 13, row.bottom), green,
                         regularFormat.Get());
            }
            const std::wstring number = std::to_wstring(index + 1) + L".";
            Win32Ui::Impl::DrawText(number, Rect(row.left + 13, row.top, row.left + 45, row.bottom),
                     track.playing ? white : greenDim, regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            std::wstring name = track.title.empty() ? L"Untitled" : track.title;
            if (showArtist && !track.artist.empty()) name = track.artist + L" - " + name;
            const auto nameBounds = Rect(row.left + 50, row.top, row.right - 82, row.bottom);
            if (trackNameEditing && trackRenameIndex == modelIndex) {
                DrawTrackRenameField(nameBounds, white);
            } else {
                Win32Ui::Impl::DrawText(name, nameBounds, selected || track.playing ? white : green,
                           regularFormat.Get());
            }
            Win32Ui::Impl::DrawText(FormatTime(track.durationSeconds), Rect(row.right - 78, row.top, row.right - 28, row.bottom),
                      selected || track.playing ? white : green, regularFormat.Get(),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
            Win32Ui::Impl::DrawTrackCover(track, row);
            HitRegion hit;
            hit.bounds = row;
            hit.kind = HitKind::Track;
            hit.id = track.id;
            hit.index = modelIndex;
            hits.push_back(hit);
            if (dragging && dropTrackIndex != static_cast<std::size_t>(-1)) {
                const auto sourceIndex = SourceTrackIndex(tracks[index]);
                if (dropTrackIndex == sourceIndex) DrawTrackDropIndicator(row, false, white);
                else if (dropTrackIndex == sourceIndex + 1) DrawTrackDropIndicator(row, true, white);
            }
        }
        target->PopAxisAlignedClip();
        if (tracks.empty()) {
            Win32Ui::Impl::DrawText(L"< NO MATCHING TRACKS >", bounds, greenDim, regularFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

// Renders the current folder view: the selected folder's loose tracks first (no
    // header) followed by one separator header per subfolder section. Falls back to a
    // plain flat list when the model provides no sections. Search filters tracks by
    // title/artist/album and hides sections left empty.
    void Win32Ui::Impl::DrawSectionedTracks(const D2D1_RECT_F& bounds, std::size_t& scroll,
                             std::size_t& visibleRows,
                             ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* greenDim,
                             ID2D1Brush* selection, ID2D1Brush* white, ID2D1Brush* dim) {
        if (model.trackSections.empty()) {
            const auto& filtered = Filtered(model.tracks, playlistQuery);
            Win32Ui::Impl::DrawTrackRows(bounds, filtered, scroll, visibleRows, screen, green, greenDim,
                          selection, white, dim, true);
            return;
        }
        // Flatten into a header/track row stream, honoring the active search filter.
        if (cachedSectionRowsRevision != model.revision || cachedSectionRowsQuery != playlistQuery) {
            cachedSectionRowsRevision = model.revision;
            cachedSectionRowsQuery = playlistQuery;
            cachedSectionRows.clear();
            cachedSectionRows.reserve(model.tracks.size() + model.trackSections.size());
            for (const auto& section : model.trackSections) {
                const std::size_t first = cachedSectionRows.size();
                const std::size_t last = std::min(section.first + section.count, model.tracks.size());
                for (std::size_t i = section.first; i < last; ++i) {
                    if (Matches(model.tracks[i], playlistQuery)) {
                        cachedSectionRows.push_back({false, {}, &model.tracks[i]});
                    }
                }
                if (cachedSectionRows.size() != first && !section.label.empty()) {
                    cachedSectionRows.insert(cachedSectionRows.begin() + static_cast<std::ptrdiff_t>(first),
                                             {true, section.label, nullptr});
                }
            }
        }
        const auto& rows = cachedSectionRows;

        if (screen == currentBrushes[5]) screenBounds.push_back(bounds);
        target->FillRectangle(bounds, screen);
        visibleRows = static_cast<std::size_t>(std::max(0.0F, std::floor(Height(bounds) / kTrackRowHeight)));
        const std::size_t maximum = rows.size() > visibleRows ? rows.size() - visibleRows : 0;
        scroll = std::min(scroll, maximum);
        target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        std::size_t trackNumber = 0;
        for (std::size_t prior = 0; prior < scroll; ++prior) {
            if (!rows[prior].header) ++trackNumber;
        }
        for (std::size_t rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
            const std::size_t index = scroll + rowIndex;
            if (index >= rows.size()) break;
            const auto& row = rows[index];
            const float top = bounds.top + static_cast<float>(rowIndex) * kTrackRowHeight;
            const auto rect = Rect(bounds.left, top, bounds.right, top + kTrackRowHeight);
            if (row.header) {
                const float mid = (rect.top + rect.bottom) * 0.5F;
                target->DrawLine({rect.left + 4, mid}, {rect.left + 20, mid}, greenDim, 1.0F);
                Win32Ui::Impl::DrawText(row.label, Rect(rect.left + 24, rect.top, rect.right - 24, rect.bottom),
                         greenDim, tinyFormat.Get());
                const float textWidth = std::min(Width(rect) * 0.5F, 8.0F * row.label.size());
                target->DrawLine({rect.left + 28 + textWidth, mid}, {rect.right - 4, mid}, greenDim, 1.0F);
                continue;
            }
            const auto& track = *row.track;
            const std::size_t modelIndex = ModelTrackIndex(row.track);
            const bool selected = trackSelection.contains(modelIndex);
            const bool hot = Contains(rect, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (selected || track.playing || hot) {
                target->FillRectangle(rect, selected || track.playing ? selection
                                                                      : currentBrushes[7]);
            }
            if (track.playing) {
                Win32Ui::Impl::DrawText(L">", Rect(rect.left + 2, rect.top, rect.left + 13, rect.bottom), green,
                         regularFormat.Get());
            }
            const std::wstring number = std::to_wstring(++trackNumber) + L".";
            Win32Ui::Impl::DrawText(number, Rect(rect.left + 13, rect.top, rect.left + 45, rect.bottom),
                     track.playing ? white : greenDim, regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            std::wstring name = track.title.empty() ? L"Untitled" : track.title;
            if (!track.artist.empty()) name = track.artist + L" - " + name;
            const auto nameBounds = Rect(rect.left + 50, rect.top, rect.right - 82, rect.bottom);
            if (trackNameEditing && trackRenameIndex == modelIndex) {
                DrawTrackRenameField(nameBounds, white);
            } else {
                Win32Ui::Impl::DrawText(name, nameBounds, selected || track.playing ? white : green,
                           regularFormat.Get());
            }
            Win32Ui::Impl::DrawText(FormatTime(track.durationSeconds), Rect(rect.right - 78, rect.top, rect.right - 28, rect.bottom),
                      selected || track.playing ? white : green, regularFormat.Get(),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
            Win32Ui::Impl::DrawTrackCover(track, rect);
            HitRegion hit;
            hit.bounds = rect;
            hit.kind = HitKind::Track;
            hit.id = track.id;
            hit.index = modelIndex;
            hits.push_back(hit);
            if (dragActive && dragKind == DragKind::Track &&
                dropTrackIndex != static_cast<std::size_t>(-1)) {
                const auto sourceIndex = SourceTrackIndex(row.track);
                if (dropTrackIndex == sourceIndex) DrawTrackDropIndicator(rect, false, white);
                else if (dropTrackIndex == sourceIndex + 1) DrawTrackDropIndicator(rect, true, white);
            }
        }
        target->PopAxisAlignedClip();
        if (rows.empty()) {
            Win32Ui::Impl::DrawText(L"< NO MATCHING TRACKS >", bounds, greenDim, regularFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }






void Win32Ui::Impl::DrawMini(const D2D1_SIZE_F size,
                   std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
        Win32Ui::Impl::DrawSkinDecor(size);
        screenBounds.clear();
        panelBounds.clear();
        moduleRegions.clear();
        decorControlBounds.clear();
        deferTexts = true;
        const auto bounds = Rect(4, 4, size.width - 4, size.height - 4);
        auto content = DrawPanel(bounds, L"RIVAN // SHADE MODE", b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get());
        const auto title = Rect(bounds.left + 4, bounds.top + 4, bounds.right - 4, bounds.top + 22);
        Win32Ui::Impl::DrawWindowButton(Rect(title.right - 19, title.top + 2, title.right - 3, title.bottom - 2), L"X", 3,
                         b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawWindowButton(Rect(title.right - 38, title.top + 2, title.right - 22, title.bottom - 2), L"^", 2,
                         b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        const auto lcd = Rect(content.left + 3, content.top + 3, content.right - 3, content.top + 49);
        // SCREEN: Mini-player LCD.
        Win32Ui::Impl::DrawBevel(lcd, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        Win32Ui::Impl::DrawText(FormatTime(model.positionSeconds), Rect(lcd.left + 5, lcd.top, lcd.left + 105, lcd.bottom),
                 b[6].Get(), digitalFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::DrawText(model.nowTitle, Rect(lcd.left + 111, lcd.top + 2, lcd.right - 5, lcd.top + 25),
                 b[6].Get(), regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::DrawText(model.nowArtist, Rect(lcd.left + 111, lcd.top + 24, lcd.right - 5, lcd.bottom - 2),
                 b[8].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        const float seekTop = lcd.bottom + 5;
        const float progress = model.durationSeconds > 0.0
            ? static_cast<float>(model.positionSeconds / model.durationSeconds) : 0.0F;
        Win32Ui::Impl::DrawSlider(Rect(content.left + 4, seekTop, content.right - 4, seekTop + 15), progress,
                   HitKind::Seek, b[5].Get(), b[13].Get(), b[2].Get(), b[3].Get(), b[4].Get());
        const float buttonTop = content.bottom - 29;
        Win32Ui::Impl::DrawButton(Rect(content.left + 4, buttonTop, content.left + 48, content.bottom - 3), L"|<<",
                   Command::Previous, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawButton(Rect(content.left + 52, buttonTop, content.left + 105, content.bottom - 3),
                   model.playback == PlaybackState::Playing ? L"||" : L">", Command::PlayPause,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get(),
                   model.playback == PlaybackState::Playing);
        Win32Ui::Impl::DrawButton(Rect(content.left + 109, buttonTop, content.left + 153, content.bottom - 3), L">>|",
                   Command::Next, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawText(L"VOL", Rect(content.right - 181, buttonTop, content.right - 151, content.bottom - 3),
                 b[13].Get(), tinyFormat.Get());
        Win32Ui::Impl::DrawSlider(Rect(content.right - 148, buttonTop + 5, content.right - 4, content.bottom - 8), model.volume,
                   HitKind::Volume, b[5].Get(), b[13].Get(), b[2].Get(), b[3].Get(), b[4].Get());
        Win32Ui::Impl::DrawSkinDecor(size, 1);
        Win32Ui::Impl::DrawSkinDecor(size, 2);
        Win32Ui::Impl::FlushDeferredTexts();
        Win32Ui::Impl::DrawImageSelection(size);
    }

// Decodes a skin image file into a device bitmap, caching by absolute path.
[[nodiscard]] ID2D1Bitmap* Win32Ui::Impl::LoadSkinBitmap(const std::filesystem::path& relative) {
        if (!wicFactory || !target || relative.empty() || model.activeSkin.directory.empty()) {
            return nullptr;
        }
        const std::wstring key = (model.activeSkin.directory / relative).wstring();
        if (const auto found = imageCache.find(key); found != imageCache.end()) {
            return found->second.Get();
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wicFactory->CreateDecoderFromFilename(key.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, decoder.ReleaseAndGetAddressOf()))) {
            return nullptr;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf())) ||
            FAILED(wicFactory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)) ||
            FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                 bitmap.ReleaseAndGetAddressOf()))) {
            return nullptr;
        }
        auto* raw = bitmap.Get();
        imageCache[key] = std::move(bitmap);
        return raw;
    }

[[nodiscard]] std::vector<Win32Ui::Impl::DecorRef> Win32Ui::Impl::DecorOrder(const skin::Skin& value) {
        std::vector<DecorRef> result;
        result.reserve(value.images.size() + value.shapes.size());
        for (std::size_t index = 0; index < value.images.size(); ++index) {
            result.push_back({true, index, value.images[index].priority});
        }
        for (std::size_t index = 0; index < value.shapes.size(); ++index) {
            result.push_back({false, index, value.shapes[index].priority});
        }
        // Priority 1 is drawn last, making it visually topmost.
        std::stable_sort(result.begin(), result.end(), [](const DecorRef& left, const DecorRef& right) {
            return left.priority > right.priority;
        });
        return result;
    }

[[nodiscard]] const std::vector<Win32Ui::Impl::DecorRef>& Win32Ui::Impl::CachedDecorOrder() {
        if (decorOrderRevision != model.revision) {
            decorOrder = DecorOrder(model.activeSkin);
            decorOrderRevision = model.revision;
        }
        return decorOrder;
    }

// Layer 0 draws on window background. Layers 1 and 2 replay enabled decor over
    // panels and screens while control holes keep sliders usable and visible.
void Win32Ui::Impl::DrawSkinDecor(const D2D1_SIZE_F size, int layer) {
        ComPtr<ID2D1PathGeometry> layerMask;
        const auto& includedBounds = layer == 1 ? panelBounds : screenBounds;
        // Panel bevel stroke is 2px and centered on the edge, so half sits outside
        // panelBounds. Expand the over-panels mask so decor covers that border too.
        const float panelMaskPad =
            layer == 1 && model.activeSkin.appearance.showPanelBorders ? 2.0F : 0.0F;
        if (layer > 0 && !includedBounds.empty() &&
            SUCCEEDED(d2dFactory->CreatePathGeometry(layerMask.ReleaseAndGetAddressOf()))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(layerMask->Open(sink.ReleaseAndGetAddressOf()))) {
                const auto addRect = [&sink](const D2D1_RECT_F& rect, float pad = 0.0F) {
                    sink->BeginFigure({rect.left - pad, rect.top - pad}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({rect.right + pad, rect.top - pad});
                    sink->AddLine({rect.right + pad, rect.bottom + pad});
                    sink->AddLine({rect.left - pad, rect.bottom + pad});
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                };
                sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
                for (const auto& bounds : includedBounds) addRect(bounds, panelMaskPad);
                if (layer == 1) {
                    for (const auto& screen : screenBounds) addRect(screen);
                }
                if (layer == 1) {
                    for (const auto& control : decorControlBounds) addRect(control);
                }
                if (SUCCEEDED(sink->Close())) {
                    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), layerMask.Get()), nullptr);
                } else {
                    layerMask.Reset();
                }
            } else {
                layerMask.Reset();
            }
        }
        if (layer > 0 && !layerMask) return;
        const auto denorm = [size](float nx, float ny) {
            return D2D1::Point2F(nx * size.width, ny * size.height);
        };
        for (const auto ref : CachedDecorOrder()) {
            if (ref.image) {
                const auto& image = model.activeSkin.images[ref.index];
                const bool overlaysLayer = layer == 0 || (layer == 1 ? image.overPanels
                                                                     : image.overScreens);
                if (!overlaysLayer) continue;
                ID2D1Bitmap* bitmap = LoadSkinBitmap(image.file);
                if (!bitmap) continue;
                const auto topLeft = denorm(image.x, image.y);
                const auto destination = Rect(topLeft.x, topLeft.y,
                                              topLeft.x + image.width * size.width,
                                              topLeft.y + image.height * size.height);
                D2D1_MATRIX_3X2_F previousTransform{};
                target->GetTransform(&previousTransform);
                const auto center = D2D1::Point2F((destination.left + destination.right) * 0.5F,
                                                 (destination.top + destination.bottom) * 0.5F);
                target->SetTransform(D2D1::Matrix3x2F::Scale(
                                         image.flipHorizontal ? -1.0F : 1.0F,
                                         image.flipVertical ? -1.0F : 1.0F, center) *
                                     D2D1::Matrix3x2F::Rotation(image.rotation, center) *
                                     previousTransform);
                const float imageOpacity = std::clamp(
                    image.opacity * model.activeSkin.appearance.backgroundImageOpacity, 0.0F, 1.0F);
                target->DrawBitmap(bitmap, destination, imageOpacity,
                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                // Soft accent wash masked by the bitmap alpha so transparent cutouts stay clear.
                if (image.tint.alpha > 0) {
                    auto tintColor = ToD2D(image.tint);
                    // Cap wash strength so the source image stays visible (accent, not full recolor).
                    tintColor.a = std::clamp(tintColor.a * imageOpacity * 0.55F, 0.0F, 1.0F);
                    if (!decorBrush) {
                        (void)target->CreateSolidColorBrush(tintColor,
                                                            decorBrush.ReleaseAndGetAddressOf());
                    }
                    if (decorBrush) {
                        decorBrush->SetColor(tintColor);
                        // FillOpacityMask requires aliased AA; restore afterward.
                        const D2D1_ANTIALIAS_MODE previousAa = target->GetAntialiasMode();
                        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                        target->FillOpacityMask(bitmap, decorBrush.Get(),
                                               D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
                                               &destination, nullptr);
                        target->SetAntialiasMode(previousAa);
                    }
                }
                target->SetTransform(previousTransform);
                continue;
            }
            const auto& shape = model.activeSkin.shapes[ref.index];
            const bool overlaysLayer = layer == 0 || (layer == 1 ? shape.overPanels
                                                                 : shape.overScreens);
            if (!overlaysLayer) continue;
            // Use RGB from shape.color; transparency comes only from shape.opacity so
            // studio 100% OPACITY is fully solid even when legacy manifests store AA < FF.
            auto color = ToD2D(shape.color);
            color.a = std::clamp(shape.opacity, 0.0F, 1.0F);
            if (!decorBrush && FAILED(target->CreateSolidColorBrush(
                    color, decorBrush.ReleaseAndGetAddressOf()))) {
                continue;
            }
            if (!decorBrush) continue;
            decorBrush->SetColor(color);
            const auto topLeft = denorm(shape.x, shape.y);
            const auto rect = Rect(topLeft.x, topLeft.y,
                                   topLeft.x + shape.width * size.width,
                                   topLeft.y + shape.height * size.height);
            const float stroke = std::max(0.5F, shape.strokeWidth);
            D2D1_MATRIX_3X2_F previousTransform{};
            target->GetTransform(&previousTransform);
            const auto center = D2D1::Point2F((rect.left + rect.right) * 0.5F,
                                               (rect.top + rect.bottom) * 0.5F);
            target->SetTransform(D2D1::Matrix3x2F::Scale(shape.flipHorizontal ? -1.0F : 1.0F,
                                                           shape.flipVertical ? -1.0F : 1.0F, center) *
                                 D2D1::Matrix3x2F::Rotation(shape.rotation, center) *
                                 previousTransform);
            switch (shape.kind) {
            case skin::ShapeKind::Rectangle:
                if (shape.filled) target->FillRectangle(rect, decorBrush.Get());
                else target->DrawRectangle(rect, decorBrush.Get(), stroke);
                break;
            case skin::ShapeKind::Ellipse: {
                const auto ellipse = D2D1::Ellipse(
                    D2D1::Point2F((rect.left + rect.right) * 0.5F, (rect.top + rect.bottom) * 0.5F),
                    Width(rect) * 0.5F, Height(rect) * 0.5F);
                if (shape.filled) target->FillEllipse(ellipse, decorBrush.Get());
                else target->DrawEllipse(ellipse, decorBrush.Get(), stroke);
                break;
            }
            case skin::ShapeKind::Line:
                target->DrawLine({rect.left, rect.top}, {rect.right, rect.bottom}, decorBrush.Get(), stroke);
                break;
            }
            target->SetTransform(previousTransform);
        }
        if (layerMask) target->PopLayer();
    }

void Win32Ui::Impl::DrawImageSelection(const D2D1_SIZE_F size) {
        if (!model.skinStudioVisible) return;
        D2D1_RECT_F bounds{};
        if (studioShapeFocused && !model.activeSkin.shapes.empty()) {
            const auto& shape = model.activeSkin.shapes[
                std::min(studioShapeIndex, model.activeSkin.shapes.size() - 1)];
            bounds = Rect(shape.x * size.width, shape.y * size.height,
                          (shape.x + shape.width) * size.width,
                          (shape.y + shape.height) * size.height);
        } else if (studioImageFocused && !model.activeSkin.images.empty()) {
            const auto& image = model.activeSkin.images[
                std::min(studioImageIndex, model.activeSkin.images.size() - 1)];
            bounds = Rect(image.x * size.width, image.y * size.height,
                          (image.x + image.width) * size.width,
                          (image.y + image.height) * size.height);
        } else {
            return;
        }
            ComPtr<ID2D1SolidColorBrush> selectionBrush;
            if (SUCCEEDED(target->CreateSolidColorBrush(ToD2D(model.activeSkin.colors.accent),
                    selectionBrush.ReleaseAndGetAddressOf()))) {
                target->DrawRectangle(bounds, selectionBrush.Get(), 1.5F);
                const auto handle = [&](float x, float y) {
                    target->FillRectangle(Rect(x - 5.0F, y - 5.0F, x + 5.0F, y + 5.0F),
                                          selectionBrush.Get());
                };
                handle(bounds.right, bounds.bottom);
                const float centerX = (bounds.left + bounds.right) * 0.5F;
                target->DrawLine({centerX, bounds.top}, {centerX, bounds.top - 18.0F},
                                 selectionBrush.Get(), 1.5F);
                target->FillEllipse(D2D1::Ellipse({centerX, bounds.top - 22.0F}, 5.0F, 5.0F),
                                    selectionBrush.Get());
        }
    }

void Win32Ui::Impl::DrawFull(const D2D1_SIZE_F size,
                   std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
        Win32Ui::Impl::DrawSkinDecor(size);
        screenBounds.clear();
        panelBounds.clear();
        moduleRegions.clear();
        decorControlBounds.clear();
        previewVideoBounds = {};
         const auto& layout = moduleGesture != ModuleGesture::None
                                  ? moduleLayoutDraft : model.moduleLayout;
          const auto boundsFor = [size](const ModuleLayoutItem& item) {
              return ModulePixelBounds(item, size);
          };
          const auto tabActive = [&layout](ModuleId id) {
              if (!layout.IsTabbed(id)) return true;
              return layout.tabOrder[layout.ActiveTabIndex()] == id;
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
             }
         };

         // Keep resize feedback visible without committing every mouse move to the
         // application/session model.
         if (moduleGesture == ModuleGesture::Resize && draggingModule) {
             if (const auto* item = layout.Find(*draggingModule)) {
                 target->DrawRectangle(boundsFor(*item), b[8].Get(), 2.0F);
             }
         }

         deferTexts = true;
          for (const auto& item : layout.items) {
              if (!item.visible || item.collapsed || !tabActive(item.id)) continue;
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
          // A tab group shares the target module's rectangle. Its tabs are painted last so
          // they remain available even when the module content contains other hit regions.
          if (layout.TabCount() > 0) {
             const auto* base = layout.Find(layout.tabOrder[0]);
             if (base) {
                 auto tabItem = *base;
                 if (moduleGesture == ModuleGesture::Move && draggingModule &&
                     layout.IsTabbed(*draggingModule)) {
                     // The dragged tab group continues to use its current target
                     // rectangle; the outline identifies the eventual drop target.
                 }
                 const auto tabBounds = boundsFor(tabItem);
                  const auto tabCount = layout.TabCount();
                  const float tabWidth = std::max(44.0F, Width(tabBounds) /
                                                       static_cast<float>(tabCount));
                  const auto tabFormat = [this](bool active) -> IDWriteTextFormat* {
                      return active ? headingFormat.Get() : tinyFormat.Get();
                  };
                  for (std::size_t i = 0; i < tabCount; ++i) {
                      const auto tab = Rect(tabBounds.left + tabWidth * static_cast<float>(i),
                                            tabBounds.top + 4.0F,
                                            std::min(tabBounds.right, tabBounds.left + tabWidth * static_cast<float>(i + 1)),
                                            tabBounds.top + 22.0F);
                      const bool active = i == layout.ActiveTabIndex();
                      DrawBevel(tab, active ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(), active);
                      DrawText(UiModuleRegistry::Get(layout.tabOrder[i]).Title(), tab, b[13].Get(),
                               tabFormat(active), DWRITE_TEXT_ALIGNMENT_CENTER);
                      AddIdHit(tab, HitKind::ModuleTab,
                               (static_cast<std::uint64_t>(static_cast<std::uint8_t>(layout.tabOrder[i])) << 32U) |
                                   static_cast<std::uint64_t>(i));
                  }
             }
          }
            if (moduleCollapseMode == ModuleCollapseMode::None &&
                moduleWindowDropZone == ModuleWindowDropZone::None && moduleDropTarget) {
                if (const auto* targetModule = layout.Find(layout.TabRoot(*moduleDropTarget))) {
                   const auto targetBounds = boundsFor(*targetModule);
                   D2D1_RECT_F indication = targetBounds;
                   switch (moduleDropZone) {
                   case ModuleDropZone::Center:
                       break;
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
                     // The expanded rectangle is the actual destination of the module;
                     // the handle outline above identifies the arrow affordance.
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
            // Collapsible modules are represented by a small bevelled arrow handle. An
            // expanded module keeps the same handle at its target edge; a collapsed one
            // is drawn from its stored handle rectangle and contributes no module panel.
             for (const auto& item : layout.items) {
                 if (!item.visible || item.collapseMode == ModuleCollapseMode::None) continue;
                 if (item.collapsed && moduleGesture == ModuleGesture::None) {
                     // The handle is the collapsed module itself. Its expanded geometry
                     // is retained in the item metadata and is restored on click.
                 }
                D2D1_RECT_F handle{};
                handle = ModuleCollapseHandleBounds(item, size);
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

void Win32Ui::Impl::DrawSettings(const D2D1_SIZE_F size,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        hits.clear();
        const auto panel = Rect(10.0F, 10.0F, size.width - 10.0F, size.height - 10.0F);
        target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
        auto content = DrawPanel(panel, L"RIVAN PREFERENCES", b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get());
        captionRect = Rect(panel.left + 4, panel.top + 4, content.right - 72, panel.top + 22);
        Win32Ui::Impl::DrawButton(Rect(content.right - 67, content.top + 3, content.right - 3, content.top + 25), L"CLOSE",
                   Command::ToggleSettings, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[9].Get());
        const float navigationWidth = std::clamp(Width(content) * 0.24F, 145.0F, 230.0F);
        const auto navigation = Rect(content.left + 3, content.top + 31,
                                     content.left + navigationWidth, content.bottom - 3);
        // SCREEN: Preferences navigation list.
        Win32Ui::Impl::DrawBevel(navigation, b[5].Get(), b[3].Get(), b[4].Get(), true);
        const std::array categories{SettingCategory::General, SettingCategory::Appearance,
                                    SettingCategory::Discord, SettingCategory::Downloading,
                                    SettingCategory::SkinManager};
        float top = navigation.top + 5;
        for (const auto category : categories) {
            const auto row = Rect(navigation.left + 4, top, navigation.right - 4, top + 25);
            const bool selected = category == model.settingsCategory;
            if (selected) target->FillRectangle(row, b[11].Get());
            Win32Ui::Impl::DrawText(CategoryName(category), Rect(row.left + 6, row.top, row.right - 4, row.bottom),
                     selected ? b[12].Get() : b[6].Get(), regularFormat.Get());
            Win32Ui::Impl::AddSettingHit(row, category);
            top += 27;
        }
        const auto details = Rect(navigation.right + 7, navigation.top, content.right - 3, content.bottom - 3);
        settingsDetailsBounds = details;
        // SCREEN: Preferences detail pane.
        Win32Ui::Impl::DrawBevel(details, b[5].Get(), b[3].Get(), b[4].Get(), true);
        const bool integrationCategory = model.settingsCategory == SettingCategory::General ||
                                          model.settingsCategory == SettingCategory::Appearance ||
                                          model.settingsCategory == SettingCategory::Discord ||
                                         model.settingsCategory == SettingCategory::Downloading;
        if (!integrationCategory) {
            Win32Ui::Impl::DrawText(CategoryName(model.settingsCategory), Rect(details.left + 15, details.top + 13,
                     details.right - 15, details.top + 42), b[6].Get(), headingFormat.Get());
        }
        if (model.settingsCategory == SettingCategory::SkinManager) {
            Win32Ui::Impl::DrawSkinManagerPane(details, b);
            return;
        }
        settingsSkinListBounds = {};
        settingsSkinRows = 0;
        if (model.settingsCategory == SettingCategory::General ||
            model.settingsCategory == SettingCategory::Appearance ||
            model.settingsCategory == SettingCategory::Discord ||
            model.settingsCategory == SettingCategory::Downloading) {
            Win32Ui::Impl::DrawGeneralPane(details, b);
            return;
        }
        settingsScrollY = 0.0F;
        settingsContentHeight = 0.0F;
        Win32Ui::Impl::DrawText(L"Settings values and persistence are owned by the application core.",
                 Rect(details.left + 15, details.top + 53, details.right - 15, details.top + 78),
                 b[9].Get(), regularFormat.Get());
        Win32Ui::Impl::DrawText(L"This classic control surface keeps the existing validated callback seam.",
                 Rect(details.left + 15, details.top + 79, details.right - 15, details.top + 105),
                 b[10].Get(), smallFormat.Get());
        Win32Ui::Impl::DrawBevel(Rect(details.left + 15, details.bottom - 48, details.right - 15, details.bottom - 16),
                  b[1].Get(), b[3].Get(), b[4].Get());
        Win32Ui::Impl::DrawText(L"CALLBACK INTERFACE: ONLINE", Rect(details.left + 20, details.bottom - 48,
                 details.right - 20, details.bottom - 16), b[8].Get(), regularFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }

// General pane: music folder list. Show every configured root, then one empty
    // slot so the next folder can be chosen. After each choice another empty slot
    // appears (no limit). Subfolders of all roots become playlists.
    void Win32Ui::Impl::DrawGeneralPane(const D2D1_RECT_F& details,
                         std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        const float left = details.left + 15;
        const float right = details.right - 15;
        const float contentTop = details.top + 15;
        const float viewportBottom = details.bottom - 4;
        const float viewportHeight = std::max(0.0F, viewportBottom - contentTop);
        {
            const float maxScroll = std::max(0.0F, settingsContentHeight - viewportHeight);
            settingsScrollY = std::clamp(settingsScrollY, 0.0F, maxScroll);
        }

        // Clip scrolled content below the category heading.
        const auto clip = Rect(details.left + 2, contentTop, details.right - 2, viewportBottom);
        target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

        float y = contentTop - settingsScrollY;

        if (model.settingsCategory == SettingCategory::General) {
        Win32Ui::Impl::DrawText(L"WINDOWS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        const float optionWidth = (right - left - 8) * 0.5F;
        SettingsButton(Rect(left, y, left + optionWidth, y + 24),
                       model.startAtStartup ? L"START AT STARTUP: ON" : L"START AT STARTUP: OFF",
                       15, b);
        SettingsButton(Rect(left + optionWidth + 8, y, right, y + 24),
                       model.exitToTray ? L"EXIT TO TRAY: ON" : L"EXIT TO TRAY: OFF",
                       16, b);
        y += 34;

        // Action encoding: browse = 100 + index, clear = 200 + index (index 0 = primary).
        const auto field = [&](const wchar_t* caption, const std::wstring& value,
                               std::size_t index, bool allowClear) {
            if (caption != nullptr && caption[0] != L'\0') {
                Win32Ui::Impl::DrawText(caption, Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                y += 29;
            } else {
                y += 4;
            }
            const float browseW = 78.0F;
            const float clearW = allowClear ? 60.0F : 0.0F;
            const auto box = Rect(left, y, right - browseW - clearW - 8, y + 24);
            Win32Ui::Impl::DrawBevel(box, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
            Win32Ui::Impl::DrawText(value.empty() ? L"(not set)" : value,
                     Rect(box.left + 5, box.top, box.right - 4, box.bottom),
                     value.empty() ? b[10].Get() : b[6].Get(), regularFormat.Get());
            SettingsButton(Rect(right - browseW - clearW - 4, y, right - clearW - 4, y + 24),
                           L"BROWSE...", 100 + static_cast<std::uint64_t>(index), b);
            if (allowClear) {
                SettingsButton(Rect(right - clearW, y, right, y + 24), L"CLEAR",
                               200 + static_cast<std::uint64_t>(index), b);
            }
            y += 28;
        };

        const std::wstring primary =
            model.musicFolders.empty() ? std::wstring{} : model.musicFolders[0];
        field(L"MUSIC FOLDER", primary, 0, false);
        if (!primary.empty()) {
            for (std::size_t i = 1; i < model.musicFolders.size(); ++i) {
                field(L"", model.musicFolders[i], i, true);
            }
            // Empty trailing slot only after last chosen folder (no limit).
            field(L"", L"", model.musicFolders.size(), false);
        }
        y += 10;

        Win32Ui::Impl::DrawText(L"FILE PREVIEW", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.filePreviewEnabled ? L"FILE PREVIEW: ON" : L"FILE PREVIEW: OFF",
                       14, b);
        y += 34;

        Win32Ui::Impl::DrawText(L"PLAYLISTS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.duplicateAsFile ? L"DUPLICATE: COPY FILE ON DISK"
                                             : L"DUPLICATE: ADD SECOND ENTRY",
                       17, b);
        y += 26;
        Win32Ui::Impl::DrawText(model.duplicateAsFile
                     ? L"Right-click > Duplicate copies the audio file and adds the copy."
                     : L"Right-click > Duplicate adds another reference to the same track.",
                  Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;
        }

        if (model.settingsCategory == SettingCategory::Appearance) {
        Win32Ui::Impl::DrawText(L"MODULE VISIBILITY", Rect(left, y, right, y + 25),
                 b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        const auto moduleLabel = [this](ModuleId id) {
            const auto* item = model.moduleLayout.Find(id);
            const bool visible = item != nullptr && item->visible;
            std::wstring label(UiModuleRegistry::Get(id).Title());
            label += visible ? L": ON" : L": OFF";
            return label;
        };
        constexpr std::array moduleIds{
            ModuleId::Rivan, ModuleId::AllMusic,
            ModuleId::GraphicEqualizer, ModuleId::RivanLibrary,
            ModuleId::VideoPreview};
        for (std::size_t i = 0; i < moduleIds.size(); i += 2) {
            const float optionWidth = (right - left - 8.0F) * 0.5F;
            SettingsButton(Rect(left, y, left + optionWidth, y + 24),
                           moduleLabel(moduleIds[i]), 60 + i, b);
            if (i + 1 < moduleIds.size()) {
                SettingsButton(Rect(left + optionWidth + 8, y, right, y + 24),
                               moduleLabel(moduleIds[i + 1]), 60 + i + 1, b);
            }
            y += 30;
        }
        SettingsButton(Rect(left, y, right, y + 24), L"RESET MODULE LAYOUT", 65, b);
        y += 34;
         Win32Ui::Impl::DrawText(L"Drag a title to move. Center drops create tabs; side drops snap.",
                  Rect(left, y, right, y + 28), b[6].Get(), tinyFormat.Get());
        y += 36;
        Win32Ui::Impl::DrawText(L"TRACK COVERS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.trackCoverArtEnabled ? L"SONG COVERS: ON" : L"SONG COVERS: OFF",
                       22, b);
        y += 26;
        Win32Ui::Impl::DrawText(L"Small cached covers appear after titles when embedded artwork is available.",
                 Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;
        }

        if (model.settingsCategory == SettingCategory::Discord) {
        Win32Ui::Impl::DrawText(L"DISCORD", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.discordEnabled ? L"RICH PRESENCE: ON" : L"RICH PRESENCE: OFF",
                       18, b);
        y += 26;
        Win32Ui::Impl::DrawText(L"Shows the playing track in Discord. Needs Discord desktop running.",
                 Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 20;
        const float discordOptionWidth = (right - left - 8) * 0.5F;
        SettingsButton(Rect(left, y, left + discordOptionWidth, y + 24),
                       model.discordShowArtist ? L"SHOW ARTIST: ON" : L"SHOW ARTIST: OFF",
                       20, b);
        SettingsButton(Rect(left + discordOptionWidth + 8, y, right, y + 24),
                        model.discordShowImageText ? L"IMAGE TEXT: RIVAN"
                                                   : L"IMAGE TEXT: OFF",
                        21, b);
        y += 34;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.discordShowGithubButton ? L"GITHUB BUTTON: ON"
                                                     : L"GITHUB BUTTON: OFF",
                       23, b);
        y += 26;
        Win32Ui::Impl::DrawText(
            L"Visible to other users only; links to https://github.com/gyatstian/Rivan.",
            Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;

        }

        if (model.settingsCategory == SettingCategory::Downloading) {
        Win32Ui::Impl::DrawText(L"YOUTUBE", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.youtubeEnabled ? L"YOUTUBE DOWNLOADER: ON"
                                            : L"YOUTUBE DOWNLOADER: OFF",
                       4, b);
        y += 30;
        if (model.youtubeEnabled) {
            const int mode = std::clamp(model.youtubeDownloadMode, 0, 2);
            // Full-width format selector: cycles MP3 -> Original -> Video (action 9).
            static constexpr const wchar_t* kFormatLabels[] = {
                L"FORMAT: MP3 (FFMPEG)",
                L"FORMAT: ORIGINAL (M4A)",
                L"FORMAT: VIDEO (MP4)",
            };
            SettingsButton(Rect(left, y, right, y + 24), kFormatLabels[mode], 9, b);
            y += 26;
            // Inline hint clarifying the tradeoff / requirement per format.
            const wchar_t* formatHint = L"";
            if (mode == 0) {
                formatHint = model.youtubeFfmpegInstalled
                                 ? L"Transcodes to .mp3 via ffmpeg (re-encode, universal)."
                                 : L"Needs ffmpeg — install below.";
            } else if (mode == 1) {
                formatHint = L"Instant, no ffmpeg, highest fidelity. Saves .m4a/.opus.";
            } else {
                formatHint = model.youtubeFfmpegInstalled
                                 ? L"Video + audio merged to .mp4."
                                 : L"No ffmpeg: falls back to progressive .mp4.";
            }
            Win32Ui::Impl::DrawText(formatHint, Rect(left, y, right, y + 14), b[6].Get(),
                     tinyFormat.Get());
            y += 18;

            const float stepW = 28.0F;
            const auto drawQualityRow = [&](const wchar_t* caption, const wchar_t* label,
                                            std::uint64_t minusId, std::uint64_t plusId) {
                Win32Ui::Impl::DrawText(caption, Rect(left, y, right, y + 16), b[8].Get(), tinyFormat.Get());
                y += 18;
                const auto qualityBox =
                    Rect(left + stepW + 4, y, right - stepW - 4, y + 24);
                SettingsButton(Rect(left, y, left + stepW, y + 24), L"-", minusId, b);
                SettingsButton(Rect(right - stepW, y, right, y + 24), L"+", plusId, b);
                Win32Ui::Impl::DrawBevel(qualityBox, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
                Win32Ui::Impl::DrawText(label,
                         Rect(qualityBox.left + 6, qualityBox.top, qualityBox.right - 6,
                              qualityBox.bottom),
                         b[6].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
                y += 30;
            };

            // Audio quality: one shared 0-9 knob for every format. In MP3 it drives the
            // libmp3lame VBR encode; in Original/Video it selects best/mid/worst stream.
            const int q = std::clamp(model.youtubeAudioQuality, 0, 9);
            static constexpr const wchar_t* kQualityLabels[] = {
                L"0 — best audio (highest fidelity, largest)",
                L"1 — very high audio",
                L"2 — high audio",
                L"3 — good audio",
                L"4 — good audio, smaller",
                L"5 — mid audio (~≤160 kb/s)",
                L"6 — mid audio, smaller",
                L"7 — low audio",
                L"8 — low audio, smaller",
                L"9 — worst audio (smallest file)",
            };
            drawQualityRow(L"AUDIO QUALITY (lower number = better · shared by all formats)",
                           kQualityLabels[q], 7, 8);

            // Video quality only applies to the Video (mp4) format.
            if (mode == 2) {
                const int vq = std::clamp(model.youtubeMp4VideoQuality, 0, 5);
                static constexpr const wchar_t* kVideoLabels[] = {
                    L"0 — max 144p, smallest file",
                    L"1 — max 240p",
                    L"2 — max 360p",
                    L"3 — max 480p",
                    L"4 — max 720p",
                    L"5 — max 1080p, largest file",
                };
                drawQualityRow(L"VIDEO QUALITY (height cap · lower = smaller)",
                               kVideoLabels[vq], 10, 11);
            }
        }
        // Hide install buttons once the tool is present; keep while installing.
        const bool showYtInstall =
            !model.youtubeYtDlpInstalled || model.youtubeInstallingYtDlp;
        const bool showFfInstall =
            !model.youtubeFfmpegInstalled || model.youtubeInstallingFfmpeg;
        if (showYtInstall || showFfInstall) {
            const wchar_t* ytLabel = model.youtubeInstallingYtDlp ? L"INSTALLING YT-DLP..."
                                                                   : L"INSTALL YT-DLP";
            const wchar_t* ffLabel = model.youtubeInstallingFfmpeg ? L"INSTALLING FFMPEG..."
                                                                    : L"INSTALL FFMPEG";
            if (showYtInstall && showFfInstall) {
                const float toolW = (right - left - 8) * 0.5F;
                SettingsButton(Rect(left, y, left + toolW, y + 24), ytLabel, 5, b);
                SettingsButton(Rect(left + toolW + 8, y, right, y + 24), ffLabel, 6, b);
            } else if (showYtInstall) {
                SettingsButton(Rect(left, y, right, y + 24), ytLabel, 5, b);
            } else {
                SettingsButton(Rect(left, y, right, y + 24), ffLabel, 6, b);
            }
            y += 28;
        }
        }

        target->PopAxisAlignedClip();

        // y is contentTop - scroll + content height; recover full content height.
        settingsContentHeight = (y + settingsScrollY) - contentTop + 8.0F;
        const float maxScroll = std::max(0.0F, settingsContentHeight - viewportHeight);
        settingsScrollY = std::clamp(settingsScrollY, 0.0F, maxScroll);

        // Thin scrollbar track when content overflows.
        if (maxScroll > 0.5F) {
            const float trackLeft = details.right - 8.0F;
            const float trackTop = contentTop;
            const float trackBottom = viewportBottom;
            const float trackH = trackBottom - trackTop;
            Win32Ui::Impl::DrawBevel(Rect(trackLeft, trackTop, details.right - 3.0F, trackBottom),
                      b[5].Get(), b[3].Get(), b[4].Get(), true, 1.0F);
            const float thumbH = std::max(24.0F, trackH * (viewportHeight / settingsContentHeight));
            const float thumbTravel = trackH - thumbH;
            const float thumbY = trackTop + (maxScroll > 0.0F
                                                 ? (settingsScrollY / maxScroll) * thumbTravel
                                                 : 0.0F);
            target->FillRectangle(Rect(trackLeft + 1.0F, thumbY, details.right - 4.0F, thumbY + thumbH),
                                  b[8].Get());
        }
    }

// Skin Manager pane: two side-by-side actions on top (open the skin studio / open the
    // skins folder), and a scrollable-ish list of saved and built-in skins below. Clicking
    // a skin applies it immediately.
    void Win32Ui::Impl::DrawSkinManagerPane(const D2D1_RECT_F& details,
                             std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        const float left = details.left + 15;
        const float right = details.right - 15;
        const auto buttons = Rect(left, details.top + 46, right, details.top + 72);
        const float half = Width(buttons) * 0.5F;
        StudioButton(Rect(buttons.left, buttons.top, buttons.left + half - 4, buttons.bottom),
                     L"SKIN STUDIO", 100, b);
        StudioButton(Rect(buttons.left + half + 4, buttons.top, buttons.right, buttons.bottom),
                     L"SKIN FOLDER", 101, b);

        Win32Ui::Impl::DrawText(L"SAVED SKINS", Rect(left, buttons.bottom + 6, right, buttons.bottom + 22),
                  b[8].Get(), tinyFormat.Get());
        const auto list = Rect(left, buttons.bottom + 24, right, details.bottom - 8);
        settingsSkinListBounds = list;
        settingsSkinRows = static_cast<std::size_t>(
            std::max(1.0F, std::floor((Height(list) - 4.0F) / 29.0F)));
        const std::size_t skinMax =
            model.skins.size() > settingsSkinRows ? model.skins.size() - settingsSkinRows : 0;
        settingsSkinScroll = std::min(settingsSkinScroll, skinMax);
        // SCREEN: Saved-skins list.
        Win32Ui::Impl::DrawBevel(list, b[5].Get(), b[3].Get(), b[4].Get(), true);
        target->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_ALIASED);
        float rowTop = list.top + 2;
        const std::size_t last =
            std::min(model.skins.size(), settingsSkinScroll + settingsSkinRows);
        for (std::size_t index = settingsSkinScroll; index < last; ++index) {
            const auto& s = model.skins[index];
            const auto row = Rect(list.left + 2, rowTop, list.right - 2, rowTop + 27);
            const bool hot = Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (s.active) target->FillRectangle(row, b[11].Get());
            else if (hot) target->FillRectangle(row, b[7].Get());
            Win32Ui::Impl::DrawText(s.active ? L"\u25B6" : (s.builtIn ? L"\u2302" : L"\u2022"),
                     Rect(row.left + 4, row.top, row.left + 20, row.bottom),
                     s.active ? b[12].Get() : b[6].Get(), regularFormat.Get());
            const float actionsLeft = s.builtIn ? row.right : row.right - 150.0F;
            Win32Ui::Impl::DrawText(s.name, Rect(row.left + 22, row.top, actionsLeft - 4, row.bottom),
                      s.active ? b[12].Get() : b[9].Get(), regularFormat.Get());
            if (!s.builtIn) {
                const float buttonWidth = 46.0F;
                StudioButton(Rect(actionsLeft, row.top + 3, actionsLeft + buttonWidth, row.bottom - 3),
                             L"RENAME", 600 + index, b);
                StudioButton(Rect(actionsLeft + 50, row.top + 3, actionsLeft + 96, row.bottom - 3),
                             L"EDIT", 700 + index, b);
                StudioButton(Rect(actionsLeft + 100, row.top + 3, row.right - 2, row.bottom - 3),
                             L"DELETE", 800 + index, b);
            }
            HitRegion hit;
            hit.bounds = Rect(row.left, row.top, actionsLeft, row.bottom);
            hit.kind = HitKind::Studio;
            hit.id = 200 + index;  // Apply skin at this index.
            hits.push_back(hit);
            rowTop += 29;
        }
        target->PopAxisAlignedClip();
        if (managerNameEditing && managerSkinIndex < model.skins.size()) {
            const auto prompt = Rect(list.left + 8, list.bottom - 34, list.right - 8, list.bottom - 7);
            target->FillRectangle(prompt, b[1].Get());
            // SCREEN: Skin rename text field.
            Win32Ui::Impl::DrawBevel(Rect(prompt.left, prompt.top, prompt.right - 58, prompt.bottom), b[5].Get(),
                      b[3].Get(), b[4].Get(), true);
            Win32Ui::Impl::DrawText(managerSkinName + (((GetTickCount64() / 500ULL) % 2ULL == 0ULL) ? L"_" : L""),
                     Rect(prompt.left + 5, prompt.top, prompt.right - 63, prompt.bottom), b[9].Get(),
                     regularFormat.Get());
            StudioButton(Rect(prompt.right - 54, prompt.top, prompt.right, prompt.bottom), L"APPLY",
                         900 + managerSkinIndex, b);
        }
        if (model.skins.empty()) {
            Win32Ui::Impl::DrawText(L"< NO SKINS INSTALLED >", list, b[10].Get(), regularFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
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

// Keeps a click-to-play mono-selection in sync with the transport. A plain track click
    // both selects and plays the row, so the played track lands in trackSelection. When the
    // transport auto-advances, the new track's `playing` flag already highlights it; without
    // this the previous row would keep its selection fill and look like it is still active.
    // Only the lone auto-selection of the previously playing row is moved; genuine multi- or
    // ctrl-selections are left untouched.
    void Win32Ui::Impl::SyncSelectionToPlayback() {
        std::size_t nowPlaying = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < model.tracks.size(); ++i) {
            if (model.tracks[i].playing) { nowPlaying = i; break; }
        }
        if (nowPlaying == lastPlayingModelIndex) return;
        // The playing row changed. If the selection is exactly the row that was playing,
        // it came from click-to-play, so hand it off to the new playing row (or drop it
        // when playback stopped) instead of stranding a stale highlight.
        if (lastPlayingModelIndex != static_cast<std::size_t>(-1) &&
            trackSelection.size() == 1 && trackSelection.contains(lastPlayingModelIndex)) {
            trackSelection.clear();
            if (nowPlaying != static_cast<std::size_t>(-1)) {
                trackSelection.insert(nowPlaying);
                trackAnchor = nowPlaying;
            } else {
                trackAnchor = static_cast<std::size_t>(-1);
            }
        }
        lastPlayingModelIndex = nowPlaying;
    }

void Win32Ui::Impl::Paint() {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        if (!CreateTarget()) {
            EndPaint(window, &paint);
            return;
        }
        const std::uint64_t previousRevision = model.revision;
        try { host.SnapshotUiModel(model); } catch (...) {}
        if (!model.trackCoverArtEnabled && !trackCoverCache.empty()) {
            trackCoverCache.clear();
            trackCoverUseCounter = 0;
            nextTrackCoverLookup = {};
        }
        // Paint only presents latest decoded preview frame. Decoder work stays off UI thread.
        if (windowKind == WindowKind::Main) SyncFilePreview();
        Win32Ui::Impl::SyncSelectionToPlayback();
        Win32Ui::Impl::ApplySkinFonts();
        hits.clear();
        colorFocusRegions.clear();
        moduleRegions.clear();
        auto& brushes = UpdateBrushes();
        target->BeginDraw();
        const auto size = target->GetSize();
        lastCanvas = size;
        if (windowKind == WindowKind::Settings) {
            Win32Ui::Impl::DrawSettings(size, brushes);
        } else if (windowKind == WindowKind::SkinStudio) {
            if (studioSection == StudioSection::Colors &&
                model.skinColorFocusRevision != seenColorFocusRevision) {
                studioColorIndex = std::min(model.focusedSkinColor, StudioColorFields().size() - 1);
                studioColorPickerVisible = true;
                studioHexEditing = false;
                studioHexSelectAll = false;
                seenColorFocusRevision = model.skinColorFocusRevision;
            }
            if (model.skinElementFocusRevision != seenElementFocusRevision) {
                const int focused = model.focusedSkinElement;
                if (focused != 0) studioSection = StudioSection::Elements;
                studioShapeFocused = focused > 0;
                studioImageFocused = focused < 0;
                if (focused > 0 && !model.activeSkin.shapes.empty()) {
                    studioShapeIndex = std::min(static_cast<std::size_t>(focused - 1),
                                                model.activeSkin.shapes.size() - 1);
                } else if (focused < 0 && !model.activeSkin.images.empty()) {
                    studioImageIndex = std::min(static_cast<std::size_t>(-focused - 1),
                                                model.activeSkin.images.size() - 1);
                }
                seenElementFocusRevision = model.skinElementFocusRevision;
            }
            if (studioOpen && !previewPending && model.revision != previousRevision) {
                studioDraft = model.activeSkin;
                if (!studioDraft.images.empty()) {
                    studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
                }
            }
            DrawSkinStudio(size, brushes);
        } else {
            if (model.skinElementFocusRevision != seenElementFocusRevision) {
                const int focused = model.focusedSkinElement;
                studioShapeFocused = focused > 0;
                studioImageFocused = focused < 0;
                if (focused > 0 && !model.activeSkin.shapes.empty()) {
                    studioShapeIndex = std::min(static_cast<std::size_t>(focused - 1),
                                                model.activeSkin.shapes.size() - 1);
                } else if (focused < 0 && !model.activeSkin.images.empty()) {
                    studioImageIndex = std::min(static_cast<std::size_t>(-focused - 1),
                                                model.activeSkin.images.size() - 1);
                }
                seenElementFocusRevision = model.skinElementFocusRevision;
            }
            if (previewFullscreen && previewIsVideo && IsVideoPreviewModuleVisible()) {
                Win32Ui::Impl::DrawPreviewFullscreenOverlay(size, brushes);
            } else {
                previewFullscreen = false;
                 // The main modules remain usable at the reduced 320x200 window size.
                 // Only explicit mini-player mode switches to the compact renderer.
                 const bool compact = model.miniPlayer;
                if (compact) DrawMini(size, brushes);
                else Win32Ui::Impl::DrawFull(size, brushes);
            }
        }
        const HRESULT result = target->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) DiscardTarget();
        EndPaint(window, &paint);
        Win32Ui::Impl::SyncRefreshTimer();
    }

void Win32Ui::Impl::Resize(UINT width, UINT height) {
        if (target && width != 0 && height != 0 &&
            FAILED(target->Resize(D2D1::SizeU(width, height)))) DiscardTarget();
        InvalidateRect(window, nullptr, FALSE);
    }

void Win32Ui::Impl::InvokeSafely(Command command) {
        try { host.Invoke(command); } catch (...) {}
        InvalidateRect(window, nullptr, FALSE);
    }

// ---- Notification-area (system tray) support ----------------------------

[[nodiscard]] NOTIFYICONDATAW Win32Ui::Impl::TrayIconData() const noexcept {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = kTrayIconId;
        return data;
    }

void Win32Ui::Impl::AddTrayIcon() {
        if (trayIconAdded || !window) return;
        NOTIFYICONDATAW data = TrayIconData();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = LoadRivanIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        lstrcpynW(data.szTip, L"Rivan", static_cast<int>(std::size(data.szTip)));
        if (Shell_NotifyIconW(NIM_ADD, &data)) trayIconAdded = true;
    }

void Win32Ui::Impl::RemoveTrayIcon() {
        if (!trayIconAdded) return;
        NOTIFYICONDATAW data = TrayIconData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayIconAdded = false;
    }

// Restores the hidden main window and drops the tray icon.
    void Win32Ui::Impl::RestoreFromTray() {
        if (window) {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        Win32Ui::Impl::RemoveTrayIcon();
    }

// Right-click tray menu: Open restores the window, Exit closes for real.
    void Win32Ui::Impl::ShowTrayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kTrayMenuOpen, L"Open Rivan");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");
        POINT cursor{};
        GetCursorPos(&cursor);
        // Required so the menu dismisses correctly when the user clicks elsewhere.
        SetForegroundWindow(window);
        const int command = static_cast<int>(TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, window, nullptr));
        DestroyMenu(menu);
        if (command == kTrayMenuOpen) {
            Win32Ui::Impl::RestoreFromTray();
        } else if (command == kTrayMenuExit) {
            Win32Ui::Impl::RemoveTrayIcon();
            DestroyWindow(window);
        }
    }

} // namespace rivan::ui
