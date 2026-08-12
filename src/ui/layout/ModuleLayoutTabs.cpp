// ModuleLayoutTabs.cpp
#include "ModuleLayout.h"

#include <algorithm>

namespace rivan::ui {

namespace {

constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

std::size_t ItemIndex(const ModuleLayout& layout, ModuleId id) noexcept {
    for (std::size_t index = 0; index < layout.items.size(); ++index) {
        if (layout.items[index].id == id) return index;
    }
    return kNoIndex;
}

bool Contains(const std::array<ModuleId, 6>& ids, std::size_t count,
              ModuleId id) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (ids[index] == id) return true;
    }
    return false;
}

bool IsLegacyTabState(const ModuleLayout& layout) noexcept {
    const auto count = layout.TabCount();
    if (count < 2) return false;
    for (std::size_t index = 0; index < count; ++index) {
        const ModuleId member = layout.tabOrder[index];
        for (std::size_t itemIndex = 0; itemIndex < layout.items.size(); ++itemIndex) {
            if (layout.items[itemIndex].id == member &&
                layout.tabGroupRoot[itemIndex] != member) {
                return false;
            }
        }
    }
    return true;
}

void CopyGeometry(ModuleLayoutItem& item, const ModuleLayoutItem& geometry) noexcept {
    item.x = geometry.x;
    item.y = geometry.y;
    item.width = geometry.width;
    item.height = geometry.height;
    if (!item.collapsed) ModuleLayout::SyncExpandedGeometry(item);
}

} // namespace

std::size_t ModuleLayout::GroupTabCount(ModuleId id) const noexcept {
    const auto itemIndex = ItemIndex(*this, id);
    if (itemIndex == kNoIndex) return 0;
    if (IsLegacyTabState(*this) && Contains(tabOrder, TabCount(), id)) {
        return TabCount();
    }
    const ModuleId root = tabGroupRoot[itemIndex];
    const auto rootIndex = ItemIndex(*this, root);
    if (rootIndex == kNoIndex || tabGroupRoot[rootIndex] != root) return 0;

    std::size_t count = 0;
    for (std::size_t index = 0; index < TabCount(); ++index) {
        const auto memberIndex = ItemIndex(*this, tabOrder[index]);
        if (memberIndex != kNoIndex && tabGroupRoot[memberIndex] == root) ++count;
    }
    return count >= 2 ? count : 0;
}

ModuleId ModuleLayout::TabRoot(ModuleId id) const noexcept {
    const auto itemIndex = ItemIndex(*this, id);
    if (itemIndex == kNoIndex || GroupTabCount(id) < 2) return id;
    if (IsLegacyTabState(*this)) return tabOrder[0];
    return tabGroupRoot[itemIndex];
}

ModuleId ModuleLayout::GroupMember(ModuleId id, std::size_t memberIndex) const noexcept {
    const ModuleId root = TabRoot(id);
    if (!IsTabbed(id)) return memberIndex == 0 ? id : root;

    std::size_t current = 0;
    for (std::size_t index = 0; index < TabCount(); ++index) {
        const ModuleId candidate = tabOrder[index];
        if (IsTabbed(candidate) && TabRoot(candidate) == root) {
            if (current == memberIndex) return candidate;
            ++current;
        }
    }
    return root;
}

std::size_t ModuleLayout::TabIndex(ModuleId id) const noexcept {
    for (std::size_t index = 0; index < TabCount(); ++index) {
        if (tabOrder[index] == id && IsTabbed(id)) return index;
    }
    return TabCount();
}

std::size_t ModuleLayout::GroupActiveTab(ModuleId id) const noexcept {
    const std::size_t count = GroupTabCount(id);
    if (count == 0) return 0;
    const auto rootIndex = ItemIndex(*this, TabRoot(id));
    if (rootIndex == kNoIndex) return 0;
    return std::min(groupActiveTab[rootIndex], count - 1);
}

ModuleId ModuleLayout::GroupActiveMember(ModuleId id) const noexcept {
    return GroupMember(id, GroupActiveTab(id));
}

void ModuleLayout::SetGroupActiveTab(ModuleId id, std::size_t index) noexcept {
    const std::size_t count = GroupTabCount(id);
    if (count == 0) return;
    const ModuleId root = TabRoot(id);
    const auto rootIndex = ItemIndex(*this, root);
    if (rootIndex == kNoIndex) return;
    groupActiveTab[rootIndex] = std::min(index, count - 1);

    // Legacy activeTab remains the active flattened index of the first group.
    if (TabCount() != 0 && TabRoot(tabOrder[0]) == root) {
        activeTab = TabIndex(GroupActiveMember(root));
    }
}

void ModuleLayout::NormalizeTabState() noexcept {
    const std::array<ModuleId, 6> previousOrder = tabOrder;
    const std::array<ModuleId, 6> previousRoots = tabGroupRoot;
    const std::array<std::size_t, 6> previousActive = groupActiveTab;
    const std::size_t previousCount = TabCount();
    const std::size_t legacyActive = activeTab;

    std::array<ModuleId, 6> ordered{};
    std::size_t orderedCount = 0;
    for (std::size_t index = 0; index < previousCount; ++index) {
        const ModuleId id = previousOrder[index];
        if (ItemIndex(*this, id) == kNoIndex || Contains(ordered, orderedCount, id)) continue;
        ordered[orderedCount++] = id;
    }

    std::array<std::size_t, 6> rootCounts{};
    for (std::size_t index = 0; index < orderedCount; ++index) {
        const auto memberIndex = ItemIndex(*this, ordered[index]);
        const ModuleId root = previousRoots[memberIndex];
        const auto rootIndex = ItemIndex(*this, root);
        if (rootIndex == kNoIndex || previousRoots[rootIndex] != root ||
            !Contains(ordered, orderedCount, root)) {
            continue;
        }
        ++rootCounts[rootIndex];
    }

    bool hasMappedGroup = false;
    for (const auto count : rootCounts) {
        if (count >= 2) {
            hasMappedGroup = true;
            break;
        }
    }
    const bool useLegacyGroup = !hasMappedGroup && orderedCount >= 2;
    const ModuleId legacyRoot = orderedCount == 0 ? ModuleId::Rivan : ordered[0];

    tabOrder = {};
    tabCount = 0;
    activeTab = 0;
    groupActiveTab = {};
    for (std::size_t index = 0; index < items.size(); ++index) {
        tabGroupRoot[index] = items[index].id;
    }

    std::array<bool, 6> emitted{};
    for (std::size_t orderedIndex = 0; orderedIndex < orderedCount; ++orderedIndex) {
        const ModuleId member = ordered[orderedIndex];
        const auto memberIndex = ItemIndex(*this, member);
        ModuleId root = legacyRoot;
        std::size_t rootIndex = ItemIndex(*this, root);
        if (!useLegacyGroup) {
            root = previousRoots[memberIndex];
            rootIndex = ItemIndex(*this, root);
            if (rootIndex == kNoIndex || rootCounts[rootIndex] < 2 || emitted[rootIndex]) continue;
        } else if (orderedIndex != 0) {
            continue;
        }
        if (rootIndex == kNoIndex || emitted[rootIndex]) continue;
        emitted[rootIndex] = true;

        std::array<ModuleId, 6> members{};
        std::size_t memberCount = 0;
        const auto append = [&members, &memberCount](ModuleId candidate) noexcept {
            if (memberCount < members.size()) members[memberCount++] = candidate;
        };
        append(root);
        for (std::size_t candidateIndex = 0; candidateIndex < orderedCount; ++candidateIndex) {
            const ModuleId candidate = ordered[candidateIndex];
            if (candidate == root) continue;
            if (useLegacyGroup || previousRoots[ItemIndex(*this, candidate)] == root) {
                append(candidate);
            }
        }
        if (memberCount < 2) continue;

        ModuleId activeMember = root;
        if (useLegacyGroup) {
            activeMember = members[std::min(legacyActive, memberCount - 1)];
        } else {
            std::size_t requestedActive = previousActive[rootIndex];
            bool anyStoredGroupActive = false;
            for (const auto value : previousActive) {
                if (value != 0) {
                    anyStoredGroupActive = true;
                    break;
                }
            }
            if (!anyStoredGroupActive && tabCount == 0 && legacyActive < memberCount) {
                requestedActive = legacyActive;
            }
            // Previous tab order may have placed root after another member. Translate
            // its local active index before root-first normalization.
            std::array<ModuleId, 6> previousMembers{};
            std::size_t previousMemberCount = 0;
            for (std::size_t candidateIndex = 0; candidateIndex < orderedCount; ++candidateIndex) {
                const ModuleId candidate = ordered[candidateIndex];
                if (previousRoots[ItemIndex(*this, candidate)] == root) {
                    previousMembers[previousMemberCount++] = candidate;
                }
            }
            if (previousMemberCount != 0) {
                activeMember = previousMembers[std::min(requestedActive, previousMemberCount - 1)];
            }
        }

        for (std::size_t index = 0; index < memberCount; ++index) {
            const auto indexInItems = ItemIndex(*this, members[index]);
            if (indexInItems != kNoIndex) tabGroupRoot[indexInItems] = root;
            if (tabCount < tabOrder.size()) tabOrder[tabCount++] = members[index];
            if (members[index] == activeMember) groupActiveTab[rootIndex] = index;
        }
    }

    if (tabCount != 0) {
        const ModuleId firstRoot = TabRoot(tabOrder[0]);
        activeTab = TabIndex(GroupActiveMember(firstRoot));
    }
}

void ModuleLayout::ClearTabs() noexcept {
    tabOrder = {};
    tabCount = 0;
    activeTab = 0;
    groupActiveTab = {};
    for (std::size_t index = 0; index < items.size(); ++index) {
        tabGroupRoot[index] = items[index].id;
    }
}

void ModuleLayout::MakeTab(ModuleId first, ModuleId second) noexcept {
    NormalizeTabState();
    if (first == second || Find(first) == nullptr || Find(second) == nullptr) return;
    if (IsTabbed(first) && TabRoot(first) == TabRoot(second)) return;

    const ModuleId root = TabRoot(first);
    const ModuleLayoutItem* geometry = Find(root);
    if (geometry == nullptr) return;

    std::array<ModuleId, 6> members{};
    std::size_t memberCount = 0;
    const auto appendGroup = [this, &members, &memberCount](ModuleId id) noexcept {
        const ModuleId groupRoot = TabRoot(id);
        const std::size_t count = GroupTabCount(id);
        if (count == 0) {
            if (!Contains(members, memberCount, id) && memberCount < members.size()) {
                members[memberCount++] = id;
            }
            return;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const ModuleId member = GroupMember(groupRoot, index);
            if (!Contains(members, memberCount, member) && memberCount < members.size()) {
                members[memberCount++] = member;
            }
        }
    };
    appendGroup(first);
    appendGroup(second);
    if (memberCount < 2) return;

    for (std::size_t index = 0; index < memberCount; ++index) {
        const ModuleId member = members[index];
        if (auto* item = Find(member)) {
            CopyGeometry(*item, *geometry);
            item->dockState = geometry->dockState;
            tabGroupRoot[ItemIndex(*this, member)] = root;
        }
    }
    groupActiveTab[ItemIndex(*this, root)] = 0;

    std::array<ModuleId, 6> remaining{};
    std::size_t remainingCount = 0;
    for (std::size_t index = 0; index < TabCount(); ++index) {
        const ModuleId member = tabOrder[index];
        if (!Contains(members, memberCount, member)) remaining[remainingCount++] = member;
    }
    tabOrder = {};
    tabCount = 0;
    for (std::size_t index = 0; index < remainingCount; ++index) tabOrder[tabCount++] = remaining[index];
    for (std::size_t index = 0; index < memberCount; ++index) {
        tabOrder[tabCount++] = members[index];
    }
    NormalizeTabState();
}

void ModuleLayout::RemoveTab(ModuleId id) noexcept {
    NormalizeTabState();
    if (!IsTabbed(id)) return;

    const ModuleId oldRoot = TabRoot(id);
    const auto* geometry = Find(oldRoot);
    if (geometry == nullptr) return;
    const ModuleLayoutItem groupGeometry = *geometry;
    const std::size_t groupCount = GroupTabCount(oldRoot);
    const std::size_t activeIndex = GroupActiveTab(oldRoot);
    const ModuleId activeMember = GroupActiveMember(oldRoot);
    std::array<ModuleId, 6> remaining{};
    std::size_t remainingCount = 0;
    for (std::size_t index = 0; index < groupCount; ++index) {
        const ModuleId member = GroupMember(oldRoot, index);
        if (member != id) remaining[remainingCount++] = member;
    }

    if (auto* removed = Find(id)) removed->dockState = ModuleDockState::Floating;
    tabGroupRoot[ItemIndex(*this, id)] = id;

    const ModuleId newRoot = remainingCount == 0 ? id : remaining[0];
    for (std::size_t index = 0; index < remainingCount; ++index) {
        if (auto* item = Find(remaining[index])) {
            CopyGeometry(*item, groupGeometry);
            tabGroupRoot[ItemIndex(*this, remaining[index])] =
                remainingCount >= 2 ? newRoot : remaining[index];
        }
    }
    groupActiveTab[ItemIndex(*this, oldRoot)] = 0;
    if (remainingCount >= 2) {
        std::size_t nextActive = 0;
        const ModuleId preservedActive = activeMember == id
            ? remaining[std::min(activeIndex, remainingCount - 1)] : activeMember;
        for (std::size_t index = 0; index < remainingCount; ++index) {
            if (remaining[index] == preservedActive) {
                nextActive = index;
                break;
            }
        }
        groupActiveTab[ItemIndex(*this, newRoot)] = nextActive;
    }

    std::array<ModuleId, 6> rebuilt{};
    std::size_t rebuiltCount = 0;
    bool replacementInserted = false;
    for (std::size_t index = 0; index < TabCount(); ++index) {
        const ModuleId member = tabOrder[index];
        if (member == id || Contains(remaining, remainingCount, member)) {
            if (!replacementInserted && remainingCount >= 2) {
                for (std::size_t memberIndex = 0; memberIndex < remainingCount; ++memberIndex) {
                    rebuilt[rebuiltCount++] = remaining[memberIndex];
                }
                replacementInserted = true;
            }
            continue;
        }
        rebuilt[rebuiltCount++] = member;
    }
    tabOrder = rebuilt;
    tabCount = rebuiltCount;
    NormalizeTabState();
}

void ModuleLayout::TabWith(ModuleId source, ModuleId target) noexcept {
    NormalizeTabState();
    if (source == target || Find(source) == nullptr || Find(target) == nullptr) return;
    if (IsTabbed(source) && TabRoot(source) == TabRoot(target)) return;

    const ModuleId root = TabRoot(target);
    const auto* geometry = Find(root);
    if (geometry == nullptr) return;
    std::array<ModuleId, 6> members{};
    std::size_t memberCount = 0;
    const auto appendGroup = [this, &members, &memberCount](ModuleId id) noexcept {
        const ModuleId groupRoot = TabRoot(id);
        const std::size_t count = GroupTabCount(id);
        if (count == 0) {
            if (!Contains(members, memberCount, id) && memberCount < members.size()) {
                members[memberCount++] = id;
            }
            return;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const ModuleId member = GroupMember(groupRoot, index);
            if (!Contains(members, memberCount, member) && memberCount < members.size()) {
                members[memberCount++] = member;
            }
        }
    };
    appendGroup(target);
    appendGroup(source);
    if (memberCount < 2) return;

    for (std::size_t index = 0; index < memberCount; ++index) {
        const ModuleId member = members[index];
        if (auto* item = Find(member)) {
            CopyGeometry(*item, *geometry);
            item->dockState = geometry->dockState;
            tabGroupRoot[ItemIndex(*this, member)] = root;
        }
    }
    for (std::size_t index = 0; index < memberCount; ++index) {
        if (members[index] == source) {
            groupActiveTab[ItemIndex(*this, root)] = index;
            break;
        }
    }

    std::array<ModuleId, 6> rebuilt{};
    std::size_t rebuiltCount = 0;
    bool mergedGroupInserted = false;
    for (std::size_t index = 0; index < TabCount(); ++index) {
        const ModuleId member = tabOrder[index];
        if (Contains(members, memberCount, member)) {
            if (!mergedGroupInserted && member == root) {
                for (std::size_t memberIndex = 0; memberIndex < memberCount; ++memberIndex) {
                    rebuilt[rebuiltCount++] = members[memberIndex];
                }
                mergedGroupInserted = true;
            }
            continue;
        }
        rebuilt[rebuiltCount++] = member;
    }
    if (!mergedGroupInserted) {
        for (std::size_t memberIndex = 0; memberIndex < memberCount; ++memberIndex) {
            rebuilt[rebuiltCount++] = members[memberIndex];
        }
    }
    tabOrder = {};
    tabCount = 0;
    for (std::size_t index = 0; index < rebuiltCount; ++index) tabOrder[tabCount++] = rebuilt[index];
    NormalizeTabState();
}

} // namespace rivan::ui
