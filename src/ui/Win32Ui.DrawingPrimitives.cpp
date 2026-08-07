// Win32Ui.DrawingPrimitives.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

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

[[nodiscard]] const Win32Ui::Impl::HitRegion* Win32Ui::Impl::HitTestContent(float x, float y) const noexcept {
    for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator) {
        if (iterator->kind != HitKind::WindowControl && Contains(iterator->bounds, x, y)) {
            return &*iterator;
        }
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
        auto layouts = std::move(deferredTextLayouts);
        deferredTextLayouts.clear();
        for (const auto& text : layouts) {
            target->DrawTextLayout(text.origin, text.layout.Get(), text.brush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
         if (!module) {
             target->FillRectangle(Rect(titleBar.left + 4, titleBar.top + 5, titleBar.left + 9,
                                        titleBar.bottom - 5), green);
         }
         const bool centered = model.activeSkin.appearance.centeredTitles;
         const auto& moduleLayout = moduleGesture != ModuleGesture::None
                                        ? moduleLayoutDraft : model.moduleLayout;
         const bool titleIsAlreadyATab = module && moduleLayout.IsTabbed(*module);
         if (!titleIsAlreadyATab) {
             const float titleLeft = module ? titleBar.left + 4 : titleBar.left + 13;
             const float titleRight = centered && !module ? titleBar.right - 13 : titleBar.right - 4;
             DrawText(titleValue,
                      Rect(titleLeft, titleBar.top, titleRight, titleBar.bottom),
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

void Win32Ui::Impl::DrawTitlebar(const D2D1_SIZE_F size,
                                 std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
         const auto bar = Rect(0.0F, 0.0F, size.width, std::min(kTitlebarHeight, size.height));
         captionRect = bar;
         titlebarControlBounds.clear();
         if (Height(bar) <= 0.0F) return;

         DrawBevel(bar, b[1].Get(), b[3].Get(), b[4].Get(), false, 1.0F);

         const bool main = windowKind == WindowKind::Main;
         const float controlsWidth = main ? 3.0F * kTitlebarButtonSize + 2.0F * 3.0F
                                          : kTitlebarButtonSize;
         const float controlsLeft = std::max(4.0F, size.width - controlsWidth - 4.0F);
         const auto title = main ? std::wstring_view(L"RIVAN")
                                 : std::wstring_view(options.title ? options.title : L"RIVAN");
         const float titleLeft = main ? 4.0F + kTitlebarButtonSize + 3.0F : 9.0F;
         DrawText(title, Rect(titleLeft, 0.0F, controlsLeft - 6.0F, bar.bottom),
                  b[13].Get(), headingFormat.Get());

         float right = size.width - 4.0F;
         const auto drawControl = [this, &right, &b](const wchar_t* label, std::uint64_t action) {
             const auto bounds = Rect(right - kTitlebarButtonSize, 3.0F,
                                      right, 3.0F + kTitlebarButtonSize);
             DrawWindowButton(bounds, label, action, b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
             titlebarControlBounds.push_back(bounds);
             right -= kTitlebarButtonSize + 3.0F;
         };

         if (main) {
             const auto settings = Rect(4.0F, 3.0F, 4.0F + kTitlebarButtonSize,
                                        3.0F + kTitlebarButtonSize);
             const bool hot = Contains(settings, static_cast<float>(titlebarMouse.x),
                                       static_cast<float>(titlebarMouse.y));
             DrawBevel(settings, hot ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get());
             DrawText(L"\u2630", settings, hot ? b[8].Get() : b[13].Get(), headingFormat.Get(),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
             AddIdHit(settings, HitKind::WindowControl, 4);
             titlebarControlBounds.push_back(settings);
         }

         drawControl(L"X", 3);
         if (main) {
             drawControl(L"^", 2);
             drawControl(L"_", 1);
         }
     }

} // namespace rivan::ui
