// ModuleTypes.h
// Stable module identities and persisted layout value types.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace rivan::ui {

enum class ModuleId : std::uint8_t {
    Rivan,
    AllMusic,
    GraphicEqualizer,
    RivanLibrary,
    VideoPreview,
};

enum class ModuleDockState : std::uint8_t {
    Floating,
    Snapped,
};

enum class ModuleDropZone : std::uint8_t {
    None,
    Center,
    Left,
    Right,
    Top,
    Bottom,
};

enum class ModuleWindowDropZone : std::uint8_t {
    None,
    RightTop,
    RightMiddle,
    RightBottom,
    Center,
    LeftTop,
    LeftMiddle,
    LeftBottom,
};

enum class ModuleCollapseMode : std::uint8_t {
    None,
    Inside,
    Outside,
};

enum class ModuleCollapseSide : std::uint8_t {
    None,
    Left,
    Right,
    Top,
    Bottom,
};

enum class ModuleExpansionBehavior : std::uint8_t {
    Squash,
    Resize,
};

struct ModuleNormalizedRect final {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

struct ModuleLayoutItem final {
    ModuleId id{ModuleId::Rivan};
    float x{};
    float y{};
    float width{1.0F};
    float height{1.0F};
    bool visible{true};
    ModuleDockState dockState{ModuleDockState::Floating};
    ModuleCollapseMode collapseMode{ModuleCollapseMode::None};
    ModuleCollapseSide collapseSide{ModuleCollapseSide::None};
    ModuleId collapseTarget{ModuleId::Rivan};
    bool collapseTargetIsWindow{};
    bool collapsed{};
    float expandedX{};
    float expandedY{};
    float expandedWidth{};
    float expandedHeight{};
    float handleX{};
    float handleY{};
    float handleWidth{};
    float handleHeight{};
};

class UiModule final {
public:
    constexpr UiModule(ModuleId id, std::string_view key, std::wstring_view title) noexcept
        : id_(id), key_(key), title_(title) {}

    [[nodiscard]] constexpr ModuleId Id() const noexcept { return id_; }
    [[nodiscard]] constexpr std::string_view Key() const noexcept { return key_; }
    [[nodiscard]] constexpr std::wstring_view Title() const noexcept { return title_; }

private:
    ModuleId id_{};
    std::string_view key_{};
    std::wstring_view title_{};
};

class UiModuleRegistry final {
public:
    [[nodiscard]] static constexpr std::span<const UiModule> Modules() noexcept {
        return kModules;
    }

    [[nodiscard]] static constexpr const UiModule* Find(ModuleId id) noexcept {
        for (const auto& module : kModules) {
            if (module.Id() == id) return &module;
        }
        return nullptr;
    }

    [[nodiscard]] static constexpr const UiModule& Get(ModuleId id) noexcept {
        if (const auto* module = Find(id)) return *module;
        return kModules.front();
    }

private:
    inline static constexpr std::array kModules{
        UiModule{ModuleId::Rivan, "rivan", L"PLAYER"},
        UiModule{ModuleId::AllMusic, "all_music", L"ALL MUSIC"},
        UiModule{ModuleId::GraphicEqualizer, "graphic_equalizer", L"GRAPHIC EQUALIZER"},
        UiModule{ModuleId::RivanLibrary, "rivan_library", L"LIBRARY"},
        UiModule{ModuleId::VideoPreview, "video_preview", L"VIDEO PREVIEW"},
    };
};

static_assert(UiModuleRegistry::Modules().size() == 5,
              "The initial main-window module catalog must contain five sections.");

} // namespace rivan::ui
