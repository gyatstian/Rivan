// ModuleLayoutInternal.h
// Internal geometry predicates shared by the ModuleLayout translation units.
#pragma once

#include "ModuleTypes.h"

#include <cmath>

namespace rivan::ui {

// Minimum normalized extent (width or height) for a usable module or canvas region.
inline constexpr float kMinimumModuleExtent = 0.10F;

// True when the rectangle has finite, ordered (right > left, bottom > top) bounds.
inline bool HasFiniteOrderedRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return std::isfinite(bounds.left) && std::isfinite(bounds.top) &&
           std::isfinite(bounds.right) && std::isfinite(bounds.bottom) &&
           bounds.right > bounds.left && bounds.bottom > bounds.top &&
           std::isfinite(bounds.right - bounds.left) &&
           std::isfinite(bounds.bottom - bounds.top);
}

// Finite ordered rectangle fully inside the normalized canvas [0, 1] x [0, 1].
inline bool HasFiniteCanvasRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return HasFiniteOrderedRectangle(bounds) && bounds.left >= 0.0F && bounds.top >= 0.0F &&
           bounds.right <= 1.0F && bounds.bottom <= 1.0F;
}

// Ordered rectangle with at least minimum module extent in both axes.
inline bool HasUsableExpandedRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return HasFiniteOrderedRectangle(bounds) &&
           bounds.right - bounds.left >= kMinimumModuleExtent &&
           bounds.bottom - bounds.top >= kMinimumModuleExtent;
}

// Finite canvas rectangle with at least minimum module extent in both axes.
inline bool HasUsableTargetRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return HasFiniteCanvasRectangle(bounds) &&
           bounds.right - bounds.left >= kMinimumModuleExtent &&
           bounds.bottom - bounds.top >= kMinimumModuleExtent;
}

// Finite canvas rectangle with a non-empty handle-sized area in both axes.
inline bool HasUsableHandleRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return HasFiniteCanvasRectangle(bounds) && bounds.right - bounds.left >= 0.001F &&
           bounds.bottom - bounds.top >= 0.001F;
}

// Finite expanded geometry whose width and height meet the minimum extent.
inline bool HasUsableExpandedGeometry(const ModuleLayoutItem& item) noexcept {
    if (!std::isfinite(item.expandedX) || !std::isfinite(item.expandedY) ||
        !std::isfinite(item.expandedWidth) || !std::isfinite(item.expandedHeight) ||
        item.expandedWidth < kMinimumModuleExtent ||
        item.expandedHeight < kMinimumModuleExtent) {
        return false;
    }
    return std::isfinite(item.expandedX + item.expandedWidth) &&
           std::isfinite(item.expandedY + item.expandedHeight);
}

} // namespace rivan::ui
