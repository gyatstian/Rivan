// ModuleLayoutTabs.cpp
#include "ModuleLayout.h"

#include <algorithm>

namespace rivan::ui {

ModuleId ModuleLayout::TabRoot(ModuleId id) const noexcept {
    if (!IsTabbed(id)) return id;
    return Find(tabOrder[0]) != nullptr ? tabOrder[0] : id;
}

void ModuleLayout::ClearTabs() noexcept {
    tabCount = 0;
    activeTab = 0;
}

void ModuleLayout::MakeTab(ModuleId first, ModuleId second) noexcept {
    ClearTabs();
    tabOrder[tabCount++] = first;
    if (first != second) tabOrder[tabCount++] = second;
    activeTab = 0;
}

void ModuleLayout::RemoveTab(ModuleId id) noexcept {
    if (!IsTabbed(id)) return;
    ModuleLayoutItem groupGeometry{};
    if (const auto* root = Find(tabOrder[0])) groupGeometry = *root;
    std::array<ModuleId, 5> remaining{};
    std::size_t remainingCount = 0;
    const auto count = TabCount();
    for (std::size_t index = 0; index < count; ++index) {
        if (tabOrder[index] != id) remaining[remainingCount++] = tabOrder[index];
    }
    for (std::size_t index = 0; index < remainingCount; ++index) {
        if (auto* item = Find(remaining[index])) {
            item->x = groupGeometry.x;
            item->y = groupGeometry.y;
            item->width = groupGeometry.width;
            item->height = groupGeometry.height;
        }
    }
    if (auto* removed = Find(id)) removed->dockState = ModuleDockState::Floating;
    if (remainingCount < 2) {
        ClearTabs();
        return;
    }
    tabOrder = remaining;
    tabCount = remainingCount;
    activeTab = std::min(activeTab, tabCount == 0 ? 0U : tabCount - 1U);
}

void ModuleLayout::TabWith(ModuleId source, ModuleId target) noexcept {
    if (source == target) return;
    if (IsTabbed(source) && IsTabbed(target) && TabRoot(source) == TabRoot(target)) return;
    const ModuleId geometryId = TabRoot(target);
    ModuleLayoutItem groupGeometry{};
    if (const auto* root = Find(geometryId)) groupGeometry = *root;
    std::array<ModuleId, 5> group{};
    std::size_t count = 0;
    const auto append = [&](ModuleId id) {
        for (std::size_t index = 0; index < count; ++index) {
            if (group[index] == id) return true;
        }
        if (count >= group.size()) return false;
        group[count++] = id;
        return true;
    };
    if (IsTabbed(target)) {
        const auto targetTabCount = TabCount();
        for (std::size_t index = 0; index < targetTabCount; ++index) {
            if (!append(tabOrder[index])) return;
        }
    } else if (!append(target)) {
        return;
    }
    if (IsTabbed(source)) {
        const auto sourceTabCount = TabCount();
        for (std::size_t index = 0; index < sourceTabCount; ++index) {
            if (!append(tabOrder[index])) return;
        }
    } else if (!append(source)) {
        return;
    }
    if (count < 2) {
        ClearTabs();
        return;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (auto* item = Find(group[index])) {
            item->x = groupGeometry.x;
            item->y = groupGeometry.y;
            item->width = groupGeometry.width;
            item->height = groupGeometry.height;
            item->dockState = Find(geometryId)
                ? Find(geometryId)->dockState : ModuleDockState::Floating;
        }
    }
    tabOrder = group;
    tabCount = count;
    for (std::size_t index = 0; index < count; ++index) {
        if (group[index] == source) {
            activeTab = index;
            break;
        }
    }
}

} // namespace rivan::ui
