// ModulePixelGeometry.cpp
#include "ModulePixelGeometry.h"

#include <d2d1helper.h>

#include <algorithm>

namespace rivan::ui {

float ModuleCollapseHandleTrackThickness(ModuleCollapseSide side, D2D1_SIZE_F size) noexcept {
    const bool verticalHandle = side == ModuleCollapseSide::Left ||
                                side == ModuleCollapseSide::Right;
    const float extent = std::max(1.0F, verticalHandle ? size.width : size.height);
    return 2.0F * kModuleCollapseHandleHeight / extent;
}

D2D1_RECT_F ModuleRawPixelBounds(const ModuleLayoutItem& item, D2D1_SIZE_F size) noexcept {
    const float width = std::max(0.0F, size.width);
    const float height = std::max(0.0F, size.height);
    const float left = item.x * width;
    const float top = item.y * height;
    const float right = (item.x + item.width) * width;
    const float bottom = (item.y + item.height) * height;
    return D2D1::RectF(left, top, std::max(left, right), std::max(top, bottom));
}

D2D1_RECT_F ModulePixelBounds(const ModuleLayoutItem& item, D2D1_SIZE_F size) noexcept {
    auto bounds = ModuleRawPixelBounds(item, size);
    constexpr float kBoundaryTolerance = 0.0001F;
    const float width = std::max(0.0F, size.width);
    const float height = std::max(0.0F, size.height);
    const float horizontalGap = std::min(kModuleWindowGap, width * 0.25F);
    const float verticalGap = std::min(kModuleWindowGap, height * 0.25F);
    if (item.x <= kBoundaryTolerance) bounds.left += horizontalGap;
    if (item.y <= kBoundaryTolerance) bounds.top += verticalGap;
    if (item.x + item.width >= 1.0F - kBoundaryTolerance) bounds.right -= horizontalGap;
    if (item.y + item.height >= 1.0F - kBoundaryTolerance) bounds.bottom -= verticalGap;
    return D2D1::RectF(bounds.left, bounds.top,
                       std::max(bounds.left, bounds.right),
                       std::max(bounds.top, bounds.bottom));
}

D2D1_RECT_F ModuleCollapseHandleBounds(const ModuleLayoutItem& item,
                                       D2D1_SIZE_F size) noexcept {
    const float width = std::max(1.0F, size.width);
    const float height = std::max(1.0F, size.height);
    const bool verticalHandle = item.collapseSide == ModuleCollapseSide::Left ||
                                item.collapseSide == ModuleCollapseSide::Right;
    const bool outsideModuleCollapse = item.collapseMode == ModuleCollapseMode::Outside &&
                                       !item.collapseTargetIsWindow;

    float left = 0.0F;
    float top = 0.0F;
    float actualWidth = 0.0F;
    float actualHeight = 0.0F;
    if (item.collapsed) {
        if (!(item.width > 0.0F) || !(item.height > 0.0F)) return {};
        const auto handleRegion = ModuleRawPixelBounds(item, size);
        const float centerX = (handleRegion.left + handleRegion.right) * 0.5F;
        const float centerY = (handleRegion.top + handleRegion.bottom) * 0.5F;
        const bool hasTargetGap = item.collapseMode == ModuleCollapseMode::Inside &&
                                  !item.collapseTargetIsWindow;
        if (hasTargetGap) {
            const float storedWidth = std::max(0.0F, handleRegion.right - handleRegion.left);
            const float storedHeight = std::max(0.0F, handleRegion.bottom - handleRegion.top);
            const float expectedWidth = verticalHandle
                ? std::min(width, kModuleCollapseHandleHeight) : storedWidth;
            const float expectedHeight = verticalHandle
                ? storedHeight : std::min(height, kModuleCollapseHandleHeight);
            actualWidth = verticalHandle
                ? (storedWidth > expectedWidth * 1.5F ? expectedWidth : storedWidth * 0.5F)
                : storedWidth;
            actualHeight = verticalHandle
                ? storedHeight
                : (storedHeight > expectedHeight * 1.5F ? expectedHeight : storedHeight * 0.5F);
        } else {
            const float expandedWidth = item.expandedWidth > 0.0F
                ? item.expandedWidth * width : 0.0F;
            const float expandedHeight = item.expandedHeight > 0.0F
                ? item.expandedHeight * height : 0.0F;
            actualWidth = verticalHandle
                ? std::min(width, kModuleCollapseHandleHeight)
                : std::min(width, expandedWidth * kModuleCollapseHandleWidthFraction);
            actualHeight = verticalHandle
                ? std::min(height, expandedHeight * kModuleCollapseHandleWidthFraction)
                : std::min(height, kModuleCollapseHandleHeight);
        }
        switch (item.collapseSide) {
        case ModuleCollapseSide::Left:
            left = outsideModuleCollapse ? handleRegion.right - actualWidth : handleRegion.left;
            top = centerY - actualHeight * 0.5F;
            break;
        case ModuleCollapseSide::Right:
            left = outsideModuleCollapse ? handleRegion.left : handleRegion.right - actualWidth;
            top = centerY - actualHeight * 0.5F;
            break;
        case ModuleCollapseSide::Top:
            left = centerX - actualWidth * 0.5F;
            top = outsideModuleCollapse ? handleRegion.bottom - actualHeight : handleRegion.top;
            break;
        case ModuleCollapseSide::Bottom:
            left = centerX - actualWidth * 0.5F;
            top = outsideModuleCollapse ? handleRegion.top : handleRegion.bottom - actualHeight;
            break;
        case ModuleCollapseSide::None:
            left = centerX - actualWidth * 0.5F;
            top = centerY - actualHeight * 0.5F;
            break;
        }
    } else {
        const auto moduleRegion = ModulePixelBounds(item, size);
        const bool hasTargetGap = item.collapseMode == ModuleCollapseMode::Inside &&
                                  !item.collapseTargetIsWindow;
        if (hasTargetGap) {
            const float storedWidth = item.handleWidth * width;
            const float storedHeight = item.handleHeight * height;
            const float expectedWidth = verticalHandle
                ? std::min(width, kModuleCollapseHandleHeight) : storedWidth;
            const float expectedHeight = verticalHandle
                ? storedHeight : std::min(height, kModuleCollapseHandleHeight);
            actualWidth = verticalHandle
                ? (storedWidth > expectedWidth * 1.5F ? expectedWidth : storedWidth * 0.5F)
                : storedWidth;
            actualHeight = verticalHandle
                ? storedHeight
                : (storedHeight > expectedHeight * 1.5F ? expectedHeight : storedHeight * 0.5F);
        } else {
            const auto rawModuleRegion = ModuleRawPixelBounds(item, size);
            const float moduleWidth = std::max(0.0F, rawModuleRegion.right - rawModuleRegion.left);
            const float moduleHeight = std::max(0.0F, rawModuleRegion.bottom - rawModuleRegion.top);
            actualWidth = verticalHandle
                ? std::min(width, kModuleCollapseHandleHeight)
                : std::min(width, moduleWidth * kModuleCollapseHandleWidthFraction);
            actualHeight = verticalHandle
                ? std::min(height, moduleHeight * kModuleCollapseHandleWidthFraction)
                : std::min(height, kModuleCollapseHandleHeight);
        }
        switch (item.collapseSide) {
        case ModuleCollapseSide::Left:
            left = outsideModuleCollapse ? moduleRegion.left - actualWidth : moduleRegion.right;
            top = (moduleRegion.top + moduleRegion.bottom - actualHeight) * 0.5F;
            break;
        case ModuleCollapseSide::Right:
            left = outsideModuleCollapse ? moduleRegion.right : moduleRegion.left - actualWidth;
            top = (moduleRegion.top + moduleRegion.bottom - actualHeight) * 0.5F;
            break;
        case ModuleCollapseSide::Top:
            left = (moduleRegion.left + moduleRegion.right - actualWidth) * 0.5F;
            top = outsideModuleCollapse ? moduleRegion.top - actualHeight : moduleRegion.bottom;
            break;
        case ModuleCollapseSide::Bottom:
            left = (moduleRegion.left + moduleRegion.right - actualWidth) * 0.5F;
            top = outsideModuleCollapse ? moduleRegion.bottom : moduleRegion.top - actualHeight;
            break;
        case ModuleCollapseSide::None:
            left = (moduleRegion.left + moduleRegion.right - actualWidth) * 0.5F;
            top = (moduleRegion.top + moduleRegion.bottom - actualHeight) * 0.5F;
            break;
        }
    }
    if (!(actualWidth > 0.0F) || !(actualHeight > 0.0F)) return {};
    left = std::clamp(left, 0.0F, std::max(0.0F, width - actualWidth));
    top = std::clamp(top, 0.0F, std::max(0.0F, height - actualHeight));
    return D2D1::RectF(left, top, left + actualWidth, top + actualHeight);
}

const wchar_t* ModuleCollapseArrow(ModuleCollapseSide side) noexcept {
    switch (side) {
    case ModuleCollapseSide::Left: return L"\u25C0";
    case ModuleCollapseSide::Right: return L"\u25B6";
    case ModuleCollapseSide::Top: return L"\u25B2";
    case ModuleCollapseSide::Bottom: return L"\u25BC";
    case ModuleCollapseSide::None: break;
    }
    return L"\u25A0";
}

const wchar_t* ModuleCollapseArrow(const ModuleLayoutItem& item) noexcept {
    ModuleCollapseSide direction = item.collapseSide;
    const bool outsideModuleCollapse = item.collapseMode == ModuleCollapseMode::Outside &&
                                       !item.collapseTargetIsWindow;
    if (item.collapsed != outsideModuleCollapse) {
        switch (direction) {
        case ModuleCollapseSide::Left: direction = ModuleCollapseSide::Right; break;
        case ModuleCollapseSide::Right: direction = ModuleCollapseSide::Left; break;
        case ModuleCollapseSide::Top: direction = ModuleCollapseSide::Bottom; break;
        case ModuleCollapseSide::Bottom: direction = ModuleCollapseSide::Top; break;
        case ModuleCollapseSide::None: break;
        }
    }
    return ModuleCollapseArrow(direction);
}

} // namespace rivan::ui
