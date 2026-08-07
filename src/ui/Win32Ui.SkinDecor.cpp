// Win32Ui.SkinDecor.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

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

} // namespace rivan::ui
