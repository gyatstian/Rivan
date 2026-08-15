// SettingsManager.Session.cpp
// Resumable window, playback, and module-layout persistence.
#include "SettingsManager.Persistence.h"

namespace rivan::config {
namespace {

core::IniDocument MakeSessionDocument(const SessionState& session) {
    core::IniDocument document;
    auto layout = session.moduleLayout;
    layout.NormalizeTabState();
    document.Set("meta", "format", "1");
    document.Set("window", "x", std::to_string(session.window.x));
    document.Set("window", "y", std::to_string(session.window.y));
    document.Set("window", "width", std::to_string(session.window.width));
    document.Set("window", "height", std::to_string(session.window.height));
    document.Set("window", "mini_mode", BoolText(session.miniMode));
    document.Set("playback", "selected_playlist", core::EncodeIniValue(session.selectedPlaylist));
    document.Set("playback", "selected_track", core::EncodeIniValue(session.selectedTrack));
    document.Set("playback", "position_ms", std::to_string(session.positionMilliseconds));
    document.Set("playback", "shuffle", BoolText(session.shuffle));
    document.Set("playback", "repeat", std::string(ToString(session.repeat)));
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        const auto& item = layout.items[i];
        const std::string key = "module_" + std::to_string(i) + "_";
        document.Set("modules", key + "x", FloatText(item.x));
        document.Set("modules", key + "y", FloatText(item.y));
        document.Set("modules", key + "width", FloatText(item.width));
        document.Set("modules", key + "height", FloatText(item.height));
        document.Set("modules", key + "visible", BoolText(item.visible));
        document.Set("modules", key + "dock", item.dockState == ui::ModuleDockState::Snapped
                                               ? "snapped" : "floating");
        document.Set("modules", key + "snap_group",
                     std::to_string(static_cast<unsigned int>(layout.snapGroup[i])));
        document.Set("modules", key + "tab_group_root",
                     std::to_string(static_cast<unsigned int>(layout.tabGroupRoot[i])));
        document.Set("modules", key + "collapse_mode",
                     std::to_string(static_cast<unsigned int>(item.collapseMode)));
        document.Set("modules", key + "collapse_side",
                     std::to_string(static_cast<unsigned int>(item.collapseSide)));
        document.Set("modules", key + "collapse_target",
                     std::to_string(static_cast<unsigned int>(item.collapseTarget)));
        document.Set("modules", key + "collapse_window", BoolText(item.collapseTargetIsWindow));
        document.Set("modules", key + "collapsed", BoolText(item.collapsed));
        document.Set("modules", key + "expanded_x", FloatText(item.expandedX));
        document.Set("modules", key + "expanded_y", FloatText(item.expandedY));
        document.Set("modules", key + "expanded_width", FloatText(item.expandedWidth));
        document.Set("modules", key + "expanded_height", FloatText(item.expandedHeight));
        document.Set("modules", key + "handle_x", FloatText(item.handleX));
        document.Set("modules", key + "handle_y", FloatText(item.handleY));
        document.Set("modules", key + "handle_width", FloatText(item.handleWidth));
        document.Set("modules", key + "handle_height", FloatText(item.handleHeight));
    }
    const ui::ModuleId legacyRoot = layout.TabCount() == 0
        ? ui::ModuleId::Rivan : layout.TabRoot(layout.tabOrder[0]);
    const std::size_t legacyCount = layout.GroupTabCount(legacyRoot);
    document.Set("modules", "tab_count", std::to_string(legacyCount));
    document.Set("modules", "active_tab", std::to_string(layout.GroupActiveTab(legacyRoot)));
    for (std::size_t i = 0; i < legacyCount; ++i) {
        document.Set("modules", "tab_" + std::to_string(i),
                     std::to_string(static_cast<unsigned int>(layout.GroupMember(legacyRoot, i))));
    }
    document.Set("modules", "tab_order_count", std::to_string(layout.TabCount()));
    for (std::size_t i = 0; i < layout.TabCount(); ++i) {
        document.Set("modules", "tab_order_" + std::to_string(i),
                     std::to_string(static_cast<unsigned int>(layout.tabOrder[i])));
    }
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        const auto root = layout.items[i].id;
        if (layout.GroupTabCount(root) < 2 || layout.TabRoot(root) != root) {
            continue;
        }
        document.Set("modules", "tab_group_" + std::to_string(static_cast<unsigned int>(root)) +
                                  "_active_tab",
                     std::to_string(layout.GroupActiveTab(root)));
    }
    return document;
}

} // namespace

SessionState SessionState::Defaults() {
    return SessionState{};
}

bool SettingsManager::LoadSession(std::string* error, std::string* warnings) {
    session_ = SessionState::Defaults();
    const auto missing = FileIsMissing(sessionFile_, error);
    if (!missing) return false;
    if (*missing) {
        if (error != nullptr) error->clear();
        return true;
    }

    auto document = core::IniDocument::Load(sessionFile_, error);
    if (!document || !ValidateFormat(*document, "Session file", error)) return false;

    ReadIntegerField(*document, "window", "x", -32768, 32767, session_.window.x, warnings);
    ReadIntegerField(*document, "window", "y", -32768, 32767, session_.window.y, warnings);
    ReadIntegerField(*document, "window", "width", 120, 16384, session_.window.width, warnings);
    ReadIntegerField(*document, "window", "height", 80, 16384, session_.window.height, warnings);
    ReadBoolField(*document, "window", "mini_mode", session_.miniMode, warnings);
    ReadEncodedString(*document, "playback", "selected_playlist", kMaximumSelectionBytes,
                      session_.selectedPlaylist, warnings);
    ReadEncodedString(*document, "playback", "selected_track", kMaximumSelectionBytes,
                      session_.selectedTrack, warnings);

    if (const auto position = document->Get("playback", "position_ms")) {
        const auto parsed = ParseInteger<std::uint64_t>(*position);
        if (!parsed || *parsed > kMaximumPositionMilliseconds) {
            AddWarning(warnings, "Ignoring invalid playback.position_ms");
        } else {
            session_.positionMilliseconds = *parsed;
        }
    }
    ReadBoolField(*document, "playback", "shuffle", session_.shuffle, warnings);
    if (const auto repeat = document->Get("playback", "repeat")) {
        const auto parsed = ParseRepeatMode(*repeat);
        if (!parsed) AddWarning(warnings, "Ignoring invalid playback.repeat");
        else session_.repeat = *parsed;
    }

    auto layout = ui::ModuleLayout::Defaults();
    const bool hasVideoPreviewGeometry = document->Get("modules", "module_4_x").has_value();
    const bool hasLyricsGeometry = document->Get("modules", "module_5_x").has_value();
    bool migratedLegacyVideoPreviewDefault = false;
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        auto& item = layout.items[i];
        const std::string key = "module_" + std::to_string(i) + "_";
        ReadFloatField(*document, "modules", key + "x", 0.0F, 1.0F, item.x, warnings);
        ReadFloatField(*document, "modules", key + "y", 0.0F, 1.0F, item.y, warnings);
        ReadFloatField(*document, "modules", key + "width", 0.001F, 1.0F, item.width, warnings);
        ReadFloatField(*document, "modules", key + "height", 0.001F, 1.0F, item.height, warnings);
        ReadBoolField(*document, "modules", key + "visible", item.visible, warnings);
        if (const auto dock = document->Get("modules", key + "dock")) {
            if (*dock == "snapped") item.dockState = ui::ModuleDockState::Snapped;
            else if (*dock == "floating") item.dockState = ui::ModuleDockState::Floating;
            else AddWarning(warnings, "Ignoring invalid modules." + key + "dock");
        }
        if (const auto snapGroup = document->Get("modules", key + "snap_group")) {
            if (const auto id = ParseInteger<unsigned int>(*snapGroup); id && *id < layout.items.size()) {
                layout.snapGroup[i] = static_cast<ui::ModuleId>(*id);
            } else AddWarning(warnings, "Ignoring invalid modules." + key + "snap_group");
        }
        if (const auto mode = document->Get("modules", key + "collapse_mode")) {
            if (const auto value = ParseInteger<unsigned int>(*mode);
                value && *value <= static_cast<unsigned int>(ui::ModuleCollapseMode::Outside)) {
                item.collapseMode = static_cast<ui::ModuleCollapseMode>(*value);
            } else AddWarning(warnings, "Ignoring invalid modules." + key + "collapse_mode");
        }
        if (const auto side = document->Get("modules", key + "collapse_side")) {
            if (const auto value = ParseInteger<unsigned int>(*side);
                value && *value <= static_cast<unsigned int>(ui::ModuleCollapseSide::Bottom)) {
                item.collapseSide = static_cast<ui::ModuleCollapseSide>(*value);
            } else AddWarning(warnings, "Ignoring invalid modules." + key + "collapse_side");
        }
        if (const auto target = document->Get("modules", key + "collapse_target")) {
            if (const auto value = ParseInteger<unsigned int>(*target);
                value && *value < layout.items.size()) {
                item.collapseTarget = static_cast<ui::ModuleId>(*value);
            } else AddWarning(warnings, "Ignoring invalid modules." + key + "collapse_target");
        }
        ReadBoolField(*document, "modules", key + "collapse_window",
                      item.collapseTargetIsWindow, warnings);
        ReadBoolField(*document, "modules", key + "collapsed", item.collapsed, warnings);
        ReadFloatField(*document, "modules", key + "expanded_x",
                       -kMaximumStoredModuleCoordinate, kMaximumStoredModuleCoordinate,
                       item.expandedX, warnings);
        ReadFloatField(*document, "modules", key + "expanded_y",
                       -kMaximumStoredModuleCoordinate, kMaximumStoredModuleCoordinate,
                       item.expandedY, warnings);
        ReadFloatField(*document, "modules", key + "expanded_width", 0.0F,
                       kMaximumStoredModuleCoordinate,
                       item.expandedWidth, warnings);
        ReadFloatField(*document, "modules", key + "expanded_height", 0.0F,
                       kMaximumStoredModuleCoordinate,
                       item.expandedHeight, warnings);
        ReadFloatField(*document, "modules", key + "handle_x", 0.0F, 1.0F,
                       item.handleX, warnings);
        ReadFloatField(*document, "modules", key + "handle_y", 0.0F, 1.0F,
                       item.handleY, warnings);
        ReadFloatField(*document, "modules", key + "handle_width", 0.0F, 1.0F,
                       item.handleWidth, warnings);
        ReadFloatField(*document, "modules", key + "handle_height", 0.0F, 1.0F,
                       item.handleHeight, warnings);
        item.x = std::min(item.x, 1.0F - item.width);
        item.y = std::min(item.y, 1.0F - item.height);
        if (!item.collapsed) ui::ModuleLayout::SyncExpandedGeometry(item);
        if (const auto root = document->Get("modules", key + "tab_group_root")) {
            if (const auto id = ParseInteger<unsigned int>(*root); id && *id < layout.items.size()) {
                layout.tabGroupRoot[i] = static_cast<ui::ModuleId>(*id);
            } else {
                AddWarning(warnings, "Ignoring invalid modules." + key + "tab_group_root");
            }
        }
    }

    // Sessions written before the standalone video-preview module used the full
    // right half for Rivan Library. Preserve custom legacy layouts by hiding the
    // new module, but split the old built-in default so upgraded users get the new
    // preview without overlapping their library.
    if (!hasVideoPreviewGeometry) {
        auto* library = layout.Find(ui::ModuleId::RivanLibrary);
        auto* videoPreview = layout.Find(ui::ModuleId::VideoPreview);
        const bool legacyDefaultLibrary = library != nullptr && library->visible &&
            !library->collapsed && std::abs(library->x - 0.46F) < 0.0001F &&
            std::abs(library->y) < 0.0001F && std::abs(library->width - 0.54F) < 0.0001F &&
            std::abs(library->height - 1.0F) < 0.0001F;
        if (legacyDefaultLibrary && videoPreview != nullptr) {
            migratedLegacyVideoPreviewDefault = true;
            const auto defaults = ui::ModuleLayout::Defaults();
            if (const auto* defaultLibrary = defaults.Find(ui::ModuleId::RivanLibrary)) {
                library->x = defaultLibrary->x;
                library->y = defaultLibrary->y;
                library->width = defaultLibrary->width;
                library->height = defaultLibrary->height;
            }
            if (const auto* defaultPreview = defaults.Find(ui::ModuleId::VideoPreview)) {
                *videoPreview = *defaultPreview;
            }
        } else if (videoPreview != nullptr) {
            videoPreview->visible = false;
            videoPreview->dockState = ui::ModuleDockState::Floating;
            videoPreview->collapseMode = ui::ModuleCollapseMode::None;
            videoPreview->collapseSide = ui::ModuleCollapseSide::None;
            videoPreview->collapseTarget = ui::ModuleId::VideoPreview;
            videoPreview->collapseTargetIsWindow = false;
            videoPreview->collapsed = false;
        }
    }

    // Sessions written before Lyrics used module_4 for Video Preview. Migrate the old
    // built-in five-module geometry to the six-panel default; retain custom layouts
    // unchanged and hide Lyrics so it never obscures a user-arranged panel.
    if (!hasLyricsGeometry) {
        const auto* library = layout.Find(ui::ModuleId::RivanLibrary);
        const auto* videoPreview = layout.Find(ui::ModuleId::VideoPreview);
        const bool oldFiveModuleDefault = library != nullptr && videoPreview != nullptr &&
            !library->collapsed && !videoPreview->collapsed && std::abs(library->x - 0.46F) < 0.0001F &&
            std::abs(library->y) < 0.0001F && std::abs(library->width - 0.54F) < 0.0001F &&
            std::abs(library->height - 0.66F) < 0.0001F &&
            std::abs(videoPreview->x - 0.46F) < 0.0001F &&
            std::abs(videoPreview->y - 0.68F) < 0.0001F &&
            std::abs(videoPreview->width - 0.54F) < 0.0001F &&
            std::abs(videoPreview->height - 0.30F) < 0.0001F;
        if (migratedLegacyVideoPreviewDefault || oldFiveModuleDefault) {
            layout = ui::ModuleLayout::Defaults();
        } else if (auto* lyrics = layout.Find(ui::ModuleId::Lyrics)) {
            lyrics->visible = false;
        }
    }

    const auto flattenedTabCount = document->Get("modules", "tab_order_count");
    if (const auto count = flattenedTabCount ? flattenedTabCount
                                             : document->Get("modules", "tab_count")) {
        if (const auto parsed = ParseInteger<std::size_t>(*count)) {
            layout.tabCount = std::min(*parsed, layout.tabOrder.size());
            for (std::size_t i = 0; i < layout.tabCount; ++i) {
                const std::string key = flattenedTabCount
                    ? "tab_order_" + std::to_string(i) : "tab_" + std::to_string(i);
                if (const auto tab = document->Get("modules", key)) {
                    if (const auto id = ParseInteger<unsigned int>(*tab); id && *id < layout.items.size()) {
                        layout.tabOrder[i] = static_cast<ui::ModuleId>(*id);
                    }
                }
            }
            for (std::size_t i = 0; i < layout.tabCount; ++i) {
                for (std::size_t j = i + 1; j < layout.tabCount; ++j) {
                    if (layout.tabOrder[i] == layout.tabOrder[j]) {
                        layout.tabCount = 0;
                        layout.activeTab = 0;
                        break;
                    }
                }
            }
        }
    }
    if (const auto active = document->Get("modules", "active_tab")) {
        if (const auto parsed = ParseInteger<std::size_t>(*active)) {
            layout.activeTab = std::min(*parsed, layout.tabCount == 0 ? 0U : layout.tabCount - 1U);
        }
    }

    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        const auto key = "tab_group_" + std::to_string(i) + "_active_tab";
        if (const auto active = document->Get("modules", key)) {
            if (const auto parsed = ParseInteger<std::size_t>(*active)) {
                layout.groupActiveTab[i] = *parsed;
            } else {
                AddWarning(warnings, "Ignoring invalid modules." + key);
            }
        }
    }
    layout.NormalizeTabState();

    for (const auto& item : layout.items) {
        std::array<ui::ModuleId, 6> path{};
        std::size_t pathCount = 0;
        ui::ModuleId current = item.id;
        while (const auto* candidate = layout.Find(current)) {
            std::size_t cycleStart = pathCount;
            for (std::size_t index = 0; index < pathCount; ++index) {
                if (path[index] == current) {
                    cycleStart = index;
                    break;
                }
            }
            if (cycleStart != pathCount) {
                for (std::size_t index = cycleStart; index < pathCount; ++index) {
                    layout.ClearModuleCollapse(path[index]);
                }
                break;
            }
            if (pathCount >= path.size()) break;
            path[pathCount++] = current;
            if (candidate->collapseMode != ui::ModuleCollapseMode::Inside ||
                candidate->collapseTargetIsWindow) {
                break;
            }
            current = layout.TabRoot(candidate->collapseTarget);
        }
    }
    session_.moduleLayout = layout;

    if (error != nullptr) error->clear();
    return true;
}

bool SettingsManager::SaveSession(std::string* error) const {
    if (!Validate(session_, error)) return false;
    return MakeSessionDocument(session_).SaveAtomic(sessionFile_, error);
}

bool SettingsManager::SetSession(SessionState candidate, std::string* error) {
    if (!Validate(candidate, error)) return false;
    session_ = std::move(candidate);
    if (error != nullptr) error->clear();
    return true;
}

void SettingsManager::ResetSession() {
    session_ = SessionState::Defaults();
}

bool SettingsManager::Validate(const SessionState& session, std::string* error) {
    if (session.window.x < -32768 || session.window.x > 32767 ||
        session.window.y < -32768 || session.window.y > 32767 ||
        session.window.width < 120 || session.window.width > 16384 ||
        session.window.height < 80 || session.window.height > 16384) {
        SetError(error, "Window rectangle is outside supported bounds");
        return false;
    }
    if (!core::IsValidUtf8(session.selectedPlaylist) ||
        session.selectedPlaylist.size() > kMaximumSelectionBytes ||
        session.selectedPlaylist.find('\0') != std::string::npos ||
        !core::IsValidUtf8(session.selectedTrack) ||
        session.selectedTrack.size() > kMaximumSelectionBytes ||
        session.selectedTrack.find('\0') != std::string::npos) {
        SetError(error, "Playlist and track selections must be valid UTF-8 up to 4096 bytes");
        return false;
    }
    if (session.positionMilliseconds > kMaximumPositionMilliseconds) {
        SetError(error, "Playback position exceeds the 30-day limit");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

std::string_view ToString(RepeatMode mode) noexcept {
    switch (mode) {
    case RepeatMode::Off: return "off";
    case RepeatMode::All: return "all";
    case RepeatMode::One: return "one";
    }
    return "off";
}

std::optional<RepeatMode> ParseRepeatMode(std::string_view value) noexcept {
    if (value == "off") return RepeatMode::Off;
    if (value == "all") return RepeatMode::All;
    if (value == "one") return RepeatMode::One;
    return std::nullopt;
}

const char* DiscordSecondaryTextText(DiscordSecondaryText mode) noexcept {
    switch (mode) {
    case DiscordSecondaryText::Off: return "off";
    case DiscordSecondaryText::SyncLyrics: return "sync_lyrics";
    case DiscordSecondaryText::TotalStreams: return "total_streams";
    }
    return "sync_lyrics";
}

std::optional<DiscordSecondaryText> ParseDiscordSecondaryText(std::string_view value) noexcept {
    if (value == "off") return DiscordSecondaryText::Off;
    if (value == "sync_lyrics") return DiscordSecondaryText::SyncLyrics;
    if (value == "total_streams") return DiscordSecondaryText::TotalStreams;
    return std::nullopt;
}

} // namespace rivan::config
