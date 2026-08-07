// ModuleDropZones.cpp
#include "ModuleDropZones.h"

#include <algorithm>
#include <cmath>

namespace rivan::ui {

bool IsWindowDrop(ModuleWindowDropZone zone) noexcept {
    return zone != ModuleWindowDropZone::None;
}

ModuleWindowDropZone ResolveModuleWindowDropZone(float x, float y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0F || x > 1.0F ||
        y < 0.0F || y > 1.0F) {
        return ModuleWindowDropZone::None;
    }
    if (x >= 0.25F && x <= 0.75F && y >= 0.25F && y <= 0.75F) {
        return ModuleWindowDropZone::Center;
    }
    const bool right = x >= 0.5F;
    if (y < 0.25F) {
        return right ? ModuleWindowDropZone::RightTop : ModuleWindowDropZone::LeftTop;
    }
    if (y > 0.75F) {
        return right ? ModuleWindowDropZone::RightBottom : ModuleWindowDropZone::LeftBottom;
    }
    return right ? ModuleWindowDropZone::RightMiddle : ModuleWindowDropZone::LeftMiddle;
}

ModuleNormalizedRect ModuleWindowDropBounds(ModuleWindowDropZone zone) noexcept {
    switch (zone) {
    case ModuleWindowDropZone::RightTop: return {0.5F, 0.0F, 1.0F, 0.5F};
    case ModuleWindowDropZone::RightMiddle: return {0.5F, 0.0F, 1.0F, 1.0F};
    case ModuleWindowDropZone::RightBottom: return {0.5F, 0.5F, 1.0F, 1.0F};
    case ModuleWindowDropZone::Center: return {0.0F, 0.0F, 1.0F, 1.0F};
    case ModuleWindowDropZone::LeftTop: return {0.0F, 0.0F, 0.5F, 0.5F};
    case ModuleWindowDropZone::LeftMiddle: return {0.0F, 0.0F, 0.5F, 1.0F};
    case ModuleWindowDropZone::LeftBottom: return {0.0F, 0.5F, 0.5F, 1.0F};
    case ModuleWindowDropZone::None: break;
    }
    return {};
}

bool IsSideDrop(ModuleDropZone zone) noexcept {
    return zone == ModuleDropZone::Left || zone == ModuleDropZone::Right ||
           zone == ModuleDropZone::Top || zone == ModuleDropZone::Bottom;
}

ModuleDropZone ResolveModuleDropZone(float x, float y, float left, float top,
                                     float right, float bottom) noexcept {
    const float width = right - left;
    const float height = bottom - top;
    if (!(width > 0.0F) || !(height > 0.0F) || x < left || x > right ||
        y < top || y > bottom) {
        return ModuleDropZone::None;
    }
    const float normalizedX = (x - left) / width;
    const float normalizedY = (y - top) / height;
    if (normalizedX >= 0.25F && normalizedX <= 0.75F &&
        normalizedY >= 0.25F && normalizedY <= 0.75F) {
        return ModuleDropZone::Center;
    }
    const float horizontalDistance = std::min(normalizedX, 1.0F - normalizedX);
    const float verticalDistance = std::min(normalizedY, 1.0F - normalizedY);
    if (horizontalDistance < verticalDistance) {
        return normalizedX < 0.5F ? ModuleDropZone::Left : ModuleDropZone::Right;
    }
    return normalizedY < 0.5F ? ModuleDropZone::Top : ModuleDropZone::Bottom;
}

} // namespace rivan::ui
