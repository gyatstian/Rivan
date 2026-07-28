// Rivan source file
// Purpose: Semantic skin data and manifest serialization.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivan::skin {

struct Color final {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;

    [[nodiscard]] constexpr std::uint32_t ToArgb() const noexcept {
        return (static_cast<std::uint32_t>(alpha) << 24U) |
               (static_cast<std::uint32_t>(red) << 16U) |
               (static_cast<std::uint32_t>(green) << 8U) |
               static_cast<std::uint32_t>(blue);
    }

    friend constexpr bool operator==(const Color&, const Color&) noexcept = default;
};

// Colors are named by UI meaning rather than by a particular control or layout.
struct SkinPalette final {
    Color windowBackground;
    Color panelBackground;
    Color raisedBackground;
    Color textPrimary;
    Color textSecondary;
    Color accent;
    Color hoverBackground;
    Color border;
    Color selection;
    Color screenBackground;
    // Controls: seek/vol fill, titlebar titles, window chrome icons, transport text, EQ ON/AUTO.
    Color playbackProgress;
    Color visualizationPrimary;
    Color visualizationSecondary;

    friend constexpr bool operator==(const SkinPalette&, const SkinPalette&) noexcept = default;
};

enum class ShapeKind : std::uint8_t {
    Rectangle,
    Ellipse,
    Line,
};

struct SkinShape final {
    ShapeKind kind{ShapeKind::Rectangle};
    // Coordinates are normalized to the full application canvas (0..1).
    float x{};
    float y{};
    float width{0.25F};
    float height{0.25F};
    bool filled{true};
    // RGB fill/stroke; alpha is always treated as opaque so studio OPACITY alone
    // controls transparency (legacy manifests may still store AA, which is ignored
    // at draw time in favor of opacity).
    Color color{255, 255, 255, 255};
    float strokeWidth{1.0F};
    float rotation{};
    float opacity{1.0F};
    bool flipHorizontal{};
    bool flipVertical{};
    bool overPanels{};
    bool overScreens{};
    std::uint8_t priority{99};

    friend constexpr bool operator==(const SkinShape&, const SkinShape&) noexcept = default;
};

// A positioned decorative image placed on the application canvas. Files are relative
// to the skin directory so packages stay portable. Rendered behind text and buttons.
// Alpha channels in the source image are honored so transparency works.
struct SkinImage final {
    std::filesystem::path file;
    // Coordinates are normalized to the full application canvas (0..1).
    float x{0.25F};
    float y{0.25F};
    float width{0.30F};
    float height{0.30F};
    float opacity{1.0F};
    float rotation{};
    bool flipHorizontal{};
    bool flipVertical{};
    bool overPanels{};
    bool overScreens{};
    std::uint8_t priority{99};
    // Optional accent tint. Alpha 0 = no tint (default). When alpha > 0, RGB is
    // blended over the bitmap as a soft color wash (not a full recolor).
    Color tint{0, 0, 0, 0};

    friend bool operator==(const SkinImage&, const SkinImage&) noexcept = default;
};

struct SkinTypography final {
    // UTF-8 DirectWrite family name. A custom font file is optional and relative to
    // the skin directory so packages remain portable.
    std::string fontFamily{"Segoe UI"};
    std::filesystem::path customFontFile;
    float baseSize{12.0F};
    float borderSize{};

    friend bool operator==(const SkinTypography&, const SkinTypography&) noexcept = default;
};

struct SkinAppearance final {
    bool transparentButtons{true};
    bool showTitleBars{false};
    bool showPanelBorders{true};
    // When true, skin images/shapes are drawn ON TOP of panels and screens (track lists,
    // LCD) instead of behind them, so decor is fully visible rather than peeking through
    // panel gaps.
    bool decorAbovePanels{false};
    std::filesystem::path backgroundImage;
    float backgroundImageOpacity{1.0F};
    float panelOpacity{1.0F};
    float screenOpacity{1.0F};
    bool centeredTitles{false};

    friend bool operator==(const SkinAppearance&, const SkinAppearance&) noexcept = default;
};

struct Skin final {
    static constexpr std::string_view BuiltInId = "dark-purple";
    static constexpr std::string_view ManifestFileName = "skin.ini";

    std::string id;
    std::string name;
    std::string author;
    std::string version;
    SkinPalette colors;
    SkinTypography typography;
    SkinAppearance appearance;
    std::vector<SkinShape> shapes;
    std::vector<SkinImage> images;
    std::filesystem::path directory;
    bool builtIn = false;

    [[nodiscard]] static Skin BuiltInDarkPurple();
    [[nodiscard]] static std::optional<Skin> LoadManifest(
        const std::filesystem::path& manifestPath,
        std::string* error = nullptr);

    [[nodiscard]] bool SaveManifestAtomic(
        const std::filesystem::path& manifestPath,
        std::string* error = nullptr) const;
    [[nodiscard]] static bool Validate(const Skin& skin, std::string* error = nullptr);
};

[[nodiscard]] std::optional<Color> ParseColor(std::string_view value) noexcept;
[[nodiscard]] std::string FormatColor(Color color);

} // namespace rivan::skin
