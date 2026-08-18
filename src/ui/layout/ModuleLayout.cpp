// ModuleLayout.cpp
// Shared geometry queries, invariants, and window-resize preservation.
#include "ModuleLayout.h"
#include "ModuleLayoutInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rivan::ui {

ModuleLayout ModuleLayout::Defaults() noexcept {
    return ModuleLayout{
        {{{ModuleId::Rivan, 0.0F, 0.0F, 1.0F, 0.32946298F, true, ModuleDockState::Snapped},
           {ModuleId::AllMusic, 0.0F, 0.27F, 0.44F, 0.45F, false, ModuleDockState::Floating},
           {ModuleId::GraphicEqualizer, 0.0F, 0.75F, 0.44F, 0.25F, false,
            ModuleDockState::Floating},
           {ModuleId::RivanLibrary, 0.0F, 0.32946298F, 1.0F, 0.670537F, true,
            ModuleDockState::Snapped},
           {ModuleId::VideoPreview, 0.46F, 0.49F, 0.54F, 0.24F, false,
            ModuleDockState::Floating},
           {ModuleId::Lyrics, 0.46F, 0.76F, 0.54F, 0.24F, false,
            ModuleDockState::Floating}}},
        {}, 0, 0,
        {ModuleId::Rivan, ModuleId::AllMusic, ModuleId::GraphicEqualizer,
          ModuleId::RivanLibrary, ModuleId::VideoPreview, ModuleId::Lyrics},
        {ModuleId::Rivan, ModuleId::AllMusic, ModuleId::GraphicEqualizer,
         ModuleId::RivanLibrary, ModuleId::VideoPreview, ModuleId::Lyrics},
        {}};
}

ModuleLayoutItem* ModuleLayout::Find(ModuleId id) noexcept {
    for (auto& item : items) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

const ModuleLayoutItem* ModuleLayout::Find(ModuleId id) const noexcept {
    for (const auto& item : items) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

bool ModuleLayout::IsTabbed(ModuleId id) const noexcept {
    return GroupTabCount(id) >= 2;
}

bool ModuleLayout::IsSnapped(ModuleId id) const noexcept {
    const auto* item = Find(id);
    return item != nullptr && item->dockState == ModuleDockState::Snapped;
}

ModuleId ModuleLayout::SnapRoot(ModuleId id) const noexcept {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index].id == id) {
            const auto candidate = snapGroup[index];
            return Find(candidate) != nullptr ? candidate : id;
        }
    }
    return id;
}

bool ModuleLayout::HasValidGeometry() const noexcept {
    for (const auto& item : items) {
        if (!std::isfinite(item.x) || !std::isfinite(item.y) ||
            !std::isfinite(item.width) || !std::isfinite(item.height) ||
            item.width <= 0.0F || item.height <= 0.0F || item.x < 0.0F || item.y < 0.0F ||
            item.x + item.width > 1.0F || item.y + item.height > 1.0F) {
            return false;
        }
        if (!item.collapsed && (item.width < kMinimumModuleExtent ||
                                item.height < kMinimumModuleExtent)) return false;
        if (item.collapsed && !HasUsableExpandedGeometry(item)) {
            return false;
        }
    }
    return true;
}

std::size_t ModuleLayout::TabCount() const noexcept {
    return std::min(tabCount, tabOrder.size());
}

ModuleNormalizedRect ModuleLayout::Bounds(const ModuleLayoutItem& item) noexcept {
    return {item.x, item.y, item.x + item.width, item.y + item.height};
}

bool ModuleLayout::Intersects(const ModuleNormalizedRect& first,
                              const ModuleNormalizedRect& second) noexcept {
    return first.left < second.right && first.right > second.left &&
           first.top < second.bottom && first.bottom > second.top;
}

bool ModuleLayout::Contains(const std::array<ModuleId, 6>& ids,
                            std::size_t count, ModuleId id) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (ids[index] == id) return true;
    }
    return false;
}

std::size_t ModuleLayout::MovingMembers(ModuleId id,
                                        std::array<ModuleId, 6>& members) const noexcept {
    std::size_t count = 0;
    const auto append = [&](ModuleId candidate) {
        if (Contains(members, count, candidate) || count >= members.size()) return;
        members[count++] = candidate;
    };
    const ModuleId tabRoot = TabRoot(id);
    const ModuleId snapRoot = SnapRoot(tabRoot);
    for (const auto& item : items) {
        if (!item.visible) continue;
        if (SnapRoot(item.id) == snapRoot) append(item.id);
        if (IsTabbed(id) && IsTabbed(item.id) && TabRoot(item.id) == tabRoot) {
            append(item.id);
        }
    }
    append(id);
    return count;
}

bool ModuleLayout::HasGeometryConflict(ModuleId firstId, ModuleId secondId) const noexcept {
    const auto* first = Find(firstId);
    const auto* second = Find(secondId);
    if (!first || !second || !first->visible || !second->visible) return false;
    if (IsTabbed(first->id) && IsTabbed(second->id) &&
        TabRoot(first->id) == TabRoot(second->id)) {
        return false;
    }
    if (IsInsideCollapseOverlap(*first, *second) || IsInsideCollapseOverlap(*second, *first)) {
        return false;
    }
    if (IsEffectivelyCollapsed(firstId) || IsEffectivelyCollapsed(secondId)) return false;
    return Intersects(Bounds(*first), Bounds(*second));
}

bool ModuleLayout::HasConflictingGeometry() const noexcept {
    for (std::size_t first = 0; first < items.size(); ++first) {
        for (std::size_t second = first + 1; second < items.size(); ++second) {
            if (HasGeometryConflict(items[first].id, items[second].id)) return true;
        }
    }
    return false;
}

bool ModuleLayout::DisableDuplicateIndependentModules() noexcept {
    constexpr float tolerance = 0.0001F;
    bool disabled = false;
    for (std::size_t first = 0; first < items.size(); ++first) {
        const auto& firstItem = items[first];
        if (!firstItem.visible) continue;
        for (std::size_t second = first + 1; second < items.size(); ++second) {
            auto& secondItem = items[second];
            if (!secondItem.visible) continue;
            if (std::abs(firstItem.x - secondItem.x) > tolerance ||
                std::abs(firstItem.y - secondItem.y) > tolerance ||
                std::abs(firstItem.width - secondItem.width) > tolerance ||
                std::abs(firstItem.height - secondItem.height) > tolerance) {
                continue;
            }
            if (!HasGeometryConflict(firstItem.id, secondItem.id)) continue;
            secondItem.visible = false;
            RemoveTab(secondItem.id);
            disabled = true;
        }
    }
    return disabled;
}

bool ModuleLayout::HasNewConflictingGeometry(const ModuleLayout& before) const noexcept {
    for (std::size_t first = 0; first < items.size(); ++first) {
        for (std::size_t second = first + 1; second < items.size(); ++second) {
            const ModuleId firstId = items[first].id;
            const ModuleId secondId = items[second].id;
            if (HasGeometryConflict(firstId, secondId) &&
                !before.HasGeometryConflict(firstId, secondId)) {
                return true;
            }
        }
    }
    return false;
}

void ModuleLayout::SyncExpandedGeometry(ModuleLayoutItem& item) noexcept {
    if (item.collapsed) return;
    item.expandedX = item.x;
    item.expandedY = item.y;
    item.expandedWidth = item.width;
    item.expandedHeight = item.height;
}

void ModuleLayout::SetTabGroupGeometry(ModuleId root, ModuleNormalizedRect bounds) noexcept {
    for (auto& candidate : items) {
        const bool belongsToTarget = candidate.id == root ||
            (IsTabbed(candidate.id) && TabRoot(candidate.id) == root);
        if (!belongsToTarget) continue;
        candidate.x = bounds.left;
        candidate.y = bounds.top;
        candidate.width = bounds.right - bounds.left;
        candidate.height = bounds.bottom - bounds.top;
        if (!candidate.collapsed) SyncExpandedGeometry(candidate);
    }
}

bool ModuleLayout::PreservePixelGeometry(float oldWidth, float oldHeight,
                                          float newWidth, float newHeight,
                                          bool resizeRight, bool resizeBottom,
                                          bool resizeLeft, bool resizeTop) noexcept {
    if (!(oldWidth > 0.0F) || !(oldHeight > 0.0F) ||
        !(newWidth > 0.0F) || !(newHeight > 0.0F)) {
        return false;
    }

    const auto preserveAxis = [this](float oldExtent, float newExtent, bool horizontal,
                                     bool resizeLeading, bool resizeTrailing) {
        if (std::abs(oldExtent - newExtent) < 0.01F) return false;

        constexpr float edgeTolerance = 0.01F;
        const bool edgeResize = resizeLeading || resizeTrailing;
        const float delta = newExtent - oldExtent;
        bool hasVisibleItem = false;
        float shift = 0.0F;
        float minimum = std::numeric_limits<float>::infinity();
        float maximum = -std::numeric_limits<float>::infinity();
        if (!edgeResize) {
            for (const auto& item : items) {
                if (!item.visible) continue;
                hasVisibleItem = true;
                const float position = (horizontal ? item.x : item.y) * oldExtent;
                const float length = (horizontal ? item.width : item.height) * oldExtent;
                minimum = std::min(minimum, position);
                maximum = std::max(maximum, position + length);
            }
            if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
                maximum - minimum > newExtent) {
                return false;
            }
            shift = std::clamp(0.0F, -minimum, newExtent - maximum);
        } else {
            for (const auto& item : items) {
                if (item.visible) {
                    hasVisibleItem = true;
                    break;
                }
            }
        }
        if (!hasVisibleItem) return false;

        const auto transform = [oldExtent, newExtent, resizeLeading, resizeTrailing,
                                edgeResize, delta, shift](float& position, float& length) {
            constexpr float edgeTolerance = 0.01F;
            float leading = position * oldExtent;
            float trailing = leading + length * oldExtent;
            if (edgeResize) {
                const bool touchesLeading = leading <= edgeTolerance;
                const bool touchesTrailing = trailing >= oldExtent - edgeTolerance;
                if (resizeLeading) {
                    leading += delta;
                    trailing += delta;
                    if (touchesLeading) leading = 0.0F;
                }
                if (resizeTrailing && touchesTrailing) trailing = newExtent;
            } else {
                leading += shift;
                trailing += shift;
            }
            position = leading / newExtent;
            length = (trailing - leading) / newExtent;
        };
        const auto transformCollapsedHandle = [oldExtent, newExtent, resizeLeading,
                                               resizeTrailing, edgeResize, delta,
                                               shift](float& position, float& length) {
            constexpr float edgeTolerance = 0.01F;
            float leading = position * oldExtent;
            const float originalLength = std::max(0.0F, length * oldExtent);
            float trailing = leading + originalLength;
            const bool touchesLeading = leading <= edgeTolerance;
            const bool touchesTrailing = trailing >= oldExtent - edgeTolerance;
            if (edgeResize) {
                if (resizeLeading) {
                    leading += delta;
                    trailing = leading + originalLength;
                    if (touchesLeading) {
                        leading = 0.0F;
                        trailing = originalLength;
                    }
                }
                if (resizeTrailing && touchesTrailing) {
                    trailing = newExtent;
                    leading = trailing - originalLength;
                }
            } else {
                leading += shift;
                trailing += shift;
            }
            const float clampedLength = std::min(originalLength, newExtent);
            leading = std::clamp(leading, 0.0F, newExtent - clampedLength);
            trailing = leading + clampedLength;
            position = leading / newExtent;
            length = clampedLength / newExtent;
        };
        const auto transformCollapsedExpanded = [oldExtent, newExtent, resizeLeading,
                                                 edgeResize, delta, shift](float& position,
                                                                            float& length) {
            constexpr float edgeTolerance = 0.01F;
            float leading = position * oldExtent;
            const float physicalLength = length * oldExtent;
            if (edgeResize) {
                if (resizeLeading) {
                    leading += delta;
                    if (leading <= edgeTolerance) leading = 0.0F;
                }
            } else {
                leading += shift;
            }
            position = leading / newExtent;
            length = physicalLength / newExtent;
        };

        ModuleLayout candidate = *this;
        for (auto& item : candidate.items) {
            if (!item.visible) continue;
            if (horizontal) {
                if (item.collapsed) {
                    transformCollapsedHandle(item.x, item.width);
                    item.handleX = item.x;
                    item.handleWidth = item.width;
                } else {
                    transform(item.x, item.width);
                    if (item.collapseMode != ModuleCollapseMode::None) {
                        transform(item.handleX, item.handleWidth);
                    }
                }
                if (item.collapsed) {
                    transformCollapsedExpanded(item.expandedX, item.expandedWidth);
                }
            } else {
                if (item.collapsed) {
                    transformCollapsedHandle(item.y, item.height);
                    item.handleY = item.y;
                    item.handleHeight = item.height;
                } else {
                    transform(item.y, item.height);
                    if (item.collapseMode != ModuleCollapseMode::None) {
                        transform(item.handleY, item.handleHeight);
                    }
                }
                if (item.collapsed) {
                    transformCollapsedExpanded(item.expandedY, item.expandedHeight);
                }
            }
            const float position = horizontal ? item.x : item.y;
            const float length = horizontal ? item.width : item.height;
            const bool outsideModuleCollapse = item.collapsed &&
                item.collapseMode == ModuleCollapseMode::Outside &&
                !item.collapseTargetIsWindow;
            if (!std::isfinite(position) || !std::isfinite(length) || !(length > 0.0F) ||
                (!outsideModuleCollapse &&
                 (position < -edgeTolerance || position + length > 1.0F + edgeTolerance))) {
                return false;
            }
            if (item.collapsed) {
                if (!HasUsableExpandedGeometry(item)) {
                    return false;
                }
            }
            if (!item.collapsed) ModuleLayout::SyncExpandedGeometry(item);
        }
        bool hasOutsideModuleCollapse = false;
        for (const auto& item : candidate.items) {
            if (item.visible && item.collapsed &&
                item.collapseMode == ModuleCollapseMode::Outside &&
                !item.collapseTargetIsWindow) {
                hasOutsideModuleCollapse = true;
                break;
            }
        }
        if (hasOutsideModuleCollapse && !candidate.ReattachOutsideCollapseHandles(*this)) {
            return false;
        }
        *this = candidate;
        return true;
    };

    const bool changedX = preserveAxis(oldWidth, newWidth, true, resizeLeft, resizeRight);
    const bool changedY = preserveAxis(oldHeight, newHeight, false, resizeTop, resizeBottom);
    return changedX || changedY;
}

bool ModuleLayout::PreserveCollapsedExpandedGeometry(
    float oldWidth, float oldHeight, float newWidth, float newHeight,
    bool /*resizeRight*/, bool /*resizeBottom*/, bool resizeLeft, bool resizeTop) noexcept {
    if (!std::isfinite(oldWidth) || !std::isfinite(oldHeight) ||
        !std::isfinite(newWidth) || !std::isfinite(newHeight) ||
        !(oldWidth > 0.0F) || !(oldHeight > 0.0F) ||
        !(newWidth > 0.0F) || !(newHeight > 0.0F)) {
        return false;
    }

    const auto preserveAxis = [](float oldExtent, float newExtent,
                                  bool resizeLeading, float& position,
                                  float& length) noexcept {
        const float physicalPosition = position * oldExtent;
        const float physicalLength = length * oldExtent;
        if (!std::isfinite(physicalPosition) || !std::isfinite(physicalLength)) {
            return false;
        }

        const float leading = physicalPosition +
            (resizeLeading ? newExtent - oldExtent : 0.0F);
        position = leading / newExtent;
        length = physicalLength / newExtent;
        return std::isfinite(position) && std::isfinite(length);
    };

    ModuleLayout candidate = *this;
    bool changed = false;
    for (auto& item : candidate.items) {
        if (!item.collapsed) continue;
        if (!HasUsableExpandedGeometry(item)) return false;

        const float oldX = item.expandedX;
        const float oldY = item.expandedY;
        const float itemOldWidth = item.expandedWidth;
        const float itemOldHeight = item.expandedHeight;
        if (!preserveAxis(oldWidth, newWidth, resizeLeft,
                          item.expandedX, item.expandedWidth) ||
            !preserveAxis(oldHeight, newHeight, resizeTop,
                          item.expandedY, item.expandedHeight) ||
            !HasUsableExpandedGeometry(item)) {
            return false;
        }
        changed = changed || item.expandedX != oldX || item.expandedY != oldY ||
            item.expandedWidth != itemOldWidth || item.expandedHeight != itemOldHeight;
    }

    if (!changed) return false;
    *this = candidate;
    return true;
}

} // namespace rivan::ui
