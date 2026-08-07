// Rivan source file
// Purpose: Semantic skin data and manifest serialization.
#include "Skin.h"

#include "../core/IniDocument.h"
#include "../core/IniValueCodec.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace rivan::skin {
namespace {

constexpr std::size_t kMaximumIdBytes = 64;
constexpr std::size_t kMaximumNameBytes = 128;
constexpr std::size_t kMaximumAuthorBytes = 128;
constexpr std::size_t kMaximumVersionBytes = 32;
constexpr std::size_t kMaximumFontBytes = 128;
constexpr std::size_t kMaximumAssetPathBytes = 260;
constexpr std::size_t kMaximumShapes = 64;
constexpr std::size_t kMaximumImages = 32;
constexpr float kMinimumFontSize = 8.0F;
constexpr float kMaximumFontSize = 32.0F;

struct PaletteField final {
    std::string_view key;
    Color SkinPalette::*member;
};

constexpr std::array kPaletteFields{
    PaletteField{"window_background", &SkinPalette::windowBackground},
    PaletteField{"panel_background", &SkinPalette::panelBackground},
    PaletteField{"raised_background", &SkinPalette::raisedBackground},
    PaletteField{"text_primary", &SkinPalette::textPrimary},
    PaletteField{"text_secondary", &SkinPalette::textSecondary},
    PaletteField{"accent", &SkinPalette::accent},
    PaletteField{"hover_background", &SkinPalette::hoverBackground},
    PaletteField{"border", &SkinPalette::border},
    PaletteField{"selection", &SkinPalette::selection},
    PaletteField{"screen_background", &SkinPalette::screenBackground},
    PaletteField{"playback_progress", &SkinPalette::playbackProgress},
    PaletteField{"visualization_primary", &SkinPalette::visualizationPrimary},
    PaletteField{"visualization_secondary", &SkinPalette::visualizationSecondary},
};

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool IsValidUtf8(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool HasInvalidTextCharacter(std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
    });
}

bool IsTrimmed(std::string_view value) noexcept {
    return !value.empty() && value.front() != ' ' && value.front() != '\t' &&
           value.back() != ' ' && value.back() != '\t';
}

bool IsId(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumIdBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_';
    });
}

bool IsMetadata(std::string_view value, std::size_t maximumBytes) noexcept {
    return value.size() <= maximumBytes && IsTrimmed(value) && IsValidUtf8(value) &&
           !HasInvalidTextCharacter(value);
}

std::optional<bool> ParseBool(std::string_view value) noexcept {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return std::nullopt;
}

std::string BoolText(bool value) {
    return value ? "true" : "false";
}

std::optional<float> ParseFloat(std::string_view value) noexcept {
    float result{};
    const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (status != std::errc{} || end != value.data() + value.size() || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::size_t> ParseSize(std::string_view value) noexcept {
    std::size_t result{};
    const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (status != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

std::string FormatFloat(float value) {
    std::array<char, 32> buffer{};
    const auto [end, status] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                             std::chars_format::general, 5);
    return status == std::errc{} ? std::string(buffer.data(), end) : "0";
}

bool IsSafeAssetPath(const std::filesystem::path& path) {
    if (path.empty()) return true;
    const auto utf8 = core::PathToUtf8(path);
    if (!utf8 || utf8->size() > kMaximumAssetPathBytes || path.is_absolute() || path.has_root_path()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == L".." || part == L".") return false;
    }
    return true;
}

std::string_view ShapeKindText(ShapeKind kind) noexcept {
    switch (kind) {
    case ShapeKind::Rectangle: return "rectangle";
    case ShapeKind::Ellipse: return "ellipse";
    case ShapeKind::Line: return "line";
    }
    return "rectangle";
}

std::optional<ShapeKind> ParseShapeKind(std::string_view value) noexcept {
    if (value == "rectangle") return ShapeKind::Rectangle;
    if (value == "ellipse") return ShapeKind::Ellipse;
    if (value == "line") return ShapeKind::Line;
    return std::nullopt;
}

int HexDigit(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

std::optional<std::uint8_t> HexByte(char high, char low) noexcept {
    const int highValue = HexDigit(high);
    const int lowValue = HexDigit(low);
    if (highValue < 0 || lowValue < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>((highValue << 4) | lowValue);
}

} // namespace

Skin Skin::BuiltInDarkPurple() {
    Skin result;
    result.id = BuiltInId;
    result.name = "red";
    result.author = "Rivan";
    result.version = "1";
    result.colors = {
        .windowBackground = {10, 9, 8, 255},
        .panelBackground = {10, 9, 8, 255},
        .raisedBackground = {10, 9, 8, 255},
        .textPrimary = {242, 244, 243, 255},
        .textSecondary = {242, 244, 243, 255},
        .accent = {242, 244, 243, 255},
        .hoverBackground = {10, 9, 8, 255},
        .border = {73, 17, 28, 255},
        .selection = {10, 9, 8, 255},
        .screenBackground = {73, 17, 28, 255},
        .playbackProgress = {242, 244, 243, 255},
        .visualizationPrimary = {242, 244, 243, 255},
        .visualizationSecondary = {242, 244, 243, 255},
    };
    result.typography = {.fontFamily = "Courier New", .customFontFile = {}, .baseSize = 14.0F};
    result.appearance = {
        .transparentButtons = true,
        .showTitleBars = false,
        .showPanelBorders = true,
        .decorAbovePanels = false,
        .backgroundImage = {},
        .backgroundImageOpacity = 1.0F,
        .panelOpacity = 1.0F,
        .screenOpacity = 1.0F,
        .centeredTitles = true,
    };
    result.builtIn = true;
    return result;
}

std::optional<Skin> Skin::LoadManifest(
    const std::filesystem::path& manifestPath,
    std::string* error) {
    auto document = core::IniDocument::Load(manifestPath, error);
    if (!document) {
        return std::nullopt;
    }

    const auto format = document->Get("skin", "format");
    if (!format || *format != "1") {
        SetError(error, "Skin manifest has an unsupported or missing format");
        return std::nullopt;
    }

    const auto id = document->Get("skin", "id");
    const auto name = document->Get("skin", "name");
    const auto author = document->Get("skin", "author");
    const auto version = document->Get("skin", "version");
    if (!id || !name || !author || !version) {
        SetError(error, "Skin manifest is missing required [skin] metadata");
        return std::nullopt;
    }

    Skin result;
    result.id = *id;
    result.name = *name;
    result.author = *author;
    result.version = *version;
    result.directory = manifestPath.parent_path();
    result.colors = BuiltInDarkPurple().colors;

    bool hasHoverBackground = false;
    for (const auto& field : kPaletteFields) {
        const auto text = document->Get("colors", field.key);
        const auto color = text ? ParseColor(*text) : std::nullopt;
        if (!color && (field.key == "hover_background" || field.key == "screen_background")) {
            continue;
        }
        if (!color) {
            SetError(error, "Skin manifest has a missing or invalid colors." + std::string(field.key));
            return std::nullopt;
        }
        result.colors.*(field.member) = *color;
        if (field.key == "hover_background") {
            hasHoverBackground = true;
        }
    }
    if (!hasHoverBackground) {
        result.colors.hoverBackground = result.colors.selection;
    }

    auto defaults = BuiltInDarkPurple();
    result.typography = defaults.typography;
    result.appearance = defaults.appearance;
    if (const auto font = document->Get("font", "family")) {
        result.typography.fontFamily = *font;
    }
    if (const auto file = document->Get("font", "custom_file")) {
        auto parsed = core::PathFromUtf8(*file);
        if (!parsed) {
            SetError(error, "Skin manifest has an invalid font.custom_file");
            return std::nullopt;
        }
        result.typography.customFontFile = *parsed;
    }
    if (const auto size = document->Get("font", "base_size")) {
        auto parsed = ParseFloat(*size);
        if (!parsed) {
            SetError(error, "Skin manifest has an invalid font.base_size");
            return std::nullopt;
        }
        result.typography.baseSize = *parsed;
    }
    if (const auto size = document->Get("font", "border_size")) {
        auto parsed = ParseFloat(*size);
        if (!parsed) {
            SetError(error, "Skin manifest has an invalid font.border_size");
            return std::nullopt;
        }
        result.typography.borderSize = *parsed;
    }

    auto readBool = [&](std::string_view key, bool& destination) -> bool {
        if (const auto value = document->Get("appearance", key)) {
            const auto parsed = ParseBool(*value);
            if (!parsed) {
                SetError(error, "Skin manifest has an invalid appearance." + std::string(key));
                return false;
            }
            destination = *parsed;
        }
        return true;
    };
    if (!readBool("transparent_buttons", result.appearance.transparentButtons) ||
        !readBool("show_title_bars", result.appearance.showTitleBars) ||
        !readBool("show_panel_borders", result.appearance.showPanelBorders) ||
        !readBool("decor_above_panels", result.appearance.decorAbovePanels) ||
        !readBool("centered_titles", result.appearance.centeredTitles)) {
        return std::nullopt;
    }
    if (const auto path = document->Get("appearance", "background_image")) {
        auto parsed = core::PathFromUtf8(*path);
        if (!parsed) {
            SetError(error, "Skin manifest has an invalid appearance.background_image");
            return std::nullopt;
        }
        result.appearance.backgroundImage = *parsed;
    }
    if (const auto opacity = document->Get("appearance", "background_image_opacity")) {
        auto parsed = ParseFloat(*opacity);
        if (!parsed) {
            SetError(error, "Skin manifest has an invalid appearance.background_image_opacity");
            return std::nullopt;
        }
        result.appearance.backgroundImageOpacity = *parsed;
    }
    if (const auto opacity = document->Get("appearance", "panel_opacity")) {
        auto parsed = ParseFloat(*opacity);
        if (!parsed) {
            SetError(error, "Skin manifest has an invalid appearance.panel_opacity");
            return std::nullopt;
        }
        result.appearance.panelOpacity = *parsed;
    }
    if (const auto opacity = document->Get("appearance", "screen_opacity")) {
        auto parsed = ParseFloat(*opacity);
        if (!parsed) {
            SetError(error, "Skin manifest has an invalid appearance.screen_opacity");
            return std::nullopt;
        }
        result.appearance.screenOpacity = *parsed;
    }

    if (const auto shapeCount = document->Get("shapes", "count")) {
        const auto count = ParseSize(*shapeCount);
        if (!count || *count > kMaximumShapes) {
            SetError(error, "Skin manifest has an invalid shapes.count");
            return std::nullopt;
        }
        result.shapes.reserve(*count);
        for (std::size_t index = 0; index < *count; ++index) {
            const std::string prefix = "shape" + std::to_string(index) + ".";
            auto get = [&](std::string_view key) {
                return document->Get("shapes", prefix + std::string(key));
            };
            const auto kind = get("kind");
            const auto x = get("x");
            const auto y = get("y");
            const auto width = get("width");
            const auto height = get("height");
            const auto filled = get("filled");
            const auto color = get("color");
            const auto strokeWidth = get("stroke_width");
            if (!kind || !x || !y || !width || !height || !filled || !color || !strokeWidth) {
                SetError(error, "Skin manifest is missing a shape field");
                return std::nullopt;
            }
            const auto parsedKind = ParseShapeKind(*kind);
            const auto parsedX = ParseFloat(*x);
            const auto parsedY = ParseFloat(*y);
            const auto parsedWidth = ParseFloat(*width);
            const auto parsedHeight = ParseFloat(*height);
            const auto parsedFilled = ParseBool(*filled);
            const auto parsedColor = ParseColor(*color);
            const auto parsedStroke = ParseFloat(*strokeWidth);
            if (!parsedKind || !parsedX || !parsedY || !parsedWidth || !parsedHeight ||
                !parsedFilled || !parsedColor || !parsedStroke) {
                SetError(error, "Skin manifest has an invalid shape field");
                return std::nullopt;
            }
            result.shapes.push_back({*parsedKind, *parsedX, *parsedY, *parsedWidth,
                                     *parsedHeight, *parsedFilled, *parsedColor, *parsedStroke, 0.0F});
            auto& shape = result.shapes.back();
            if (const auto rotation = get("rotation")) {
                const auto parsed = ParseFloat(*rotation);
                if (!parsed) { SetError(error, "Skin manifest has an invalid shape rotation"); return std::nullopt; }
                shape.rotation = *parsed;
            }
            if (const auto opacity = get("opacity")) {
                const auto parsed = ParseFloat(*opacity);
                if (!parsed) { SetError(error, "Skin manifest has an invalid shape opacity"); return std::nullopt; }
                shape.opacity = *parsed;
            }
            for (const auto [key, field] : {std::pair{"flip_horizontal", &SkinShape::flipHorizontal},
                                            {"flip_vertical", &SkinShape::flipVertical},
                                            {"over_panels", &SkinShape::overPanels},
                                            {"over_screens", &SkinShape::overScreens}}) {
                if (const auto value = get(key)) {
                    const auto parsed = ParseBool(*value);
                    if (!parsed) { SetError(error, "Skin manifest has an invalid shape boolean"); return std::nullopt; }
                    shape.*field = *parsed;
                }
            }
            if (const auto priority = get("priority")) {
                const auto parsed = ParseSize(*priority);
                if (!parsed || *parsed < 1 || *parsed > 99) {
                    SetError(error, "Skin manifest has an invalid shape priority");
                    return std::nullopt;
                }
                shape.priority = static_cast<std::uint8_t>(*parsed);
            }
        }
    }

    if (const auto imageCount = document->Get("images", "count")) {
        const auto count = ParseSize(*imageCount);
        if (!count || *count > kMaximumImages) {
            SetError(error, "Skin manifest has an invalid images.count");
            return std::nullopt;
        }
        result.images.reserve(*count);
        for (std::size_t index = 0; index < *count; ++index) {
            const std::string prefix = "image" + std::to_string(index) + ".";
            auto get = [&](std::string_view key) {
                return document->Get("images", prefix + std::string(key));
            };
            const auto file = get("file");
            const auto x = get("x");
            const auto y = get("y");
            const auto width = get("width");
            const auto height = get("height");
            const auto opacity = get("opacity");
            if (!file || !x || !y || !width || !height || !opacity) {
                SetError(error, "Skin manifest is missing an image field");
                return std::nullopt;
            }
            auto parsedFile = core::PathFromUtf8(*file);
            const auto parsedX = ParseFloat(*x);
            const auto parsedY = ParseFloat(*y);
            const auto parsedWidth = ParseFloat(*width);
            const auto parsedHeight = ParseFloat(*height);
            const auto parsedOpacity = ParseFloat(*opacity);
            if (!parsedFile || !parsedX || !parsedY || !parsedWidth || !parsedHeight || !parsedOpacity) {
                SetError(error, "Skin manifest has an invalid image field");
                return std::nullopt;
            }
            result.images.push_back({std::move(*parsedFile), *parsedX, *parsedY, *parsedWidth,
                                     *parsedHeight, *parsedOpacity});
            auto& image = result.images.back();
            if (const auto rotation = get("rotation")) {
                const auto parsed = ParseFloat(*rotation);
                if (!parsed) {
                    SetError(error, "Skin manifest has an invalid image rotation");
                    return std::nullopt;
                }
                image.rotation = *parsed;
            }
            if (const auto flipHorizontal = get("flip_horizontal")) {
                const auto parsed = ParseBool(*flipHorizontal);
                if (!parsed) {
                    SetError(error, "Skin manifest has an invalid image flip_horizontal");
                    return std::nullopt;
                }
                image.flipHorizontal = *parsed;
            }
            if (const auto flipVertical = get("flip_vertical")) {
                const auto parsed = ParseBool(*flipVertical);
                if (!parsed) {
                    SetError(error, "Skin manifest has an invalid image flip_vertical");
                    return std::nullopt;
                }
                image.flipVertical = *parsed;
            }
            if (const auto tint = get("tint")) {
                const auto parsed = ParseColor(*tint);
                if (!parsed) {
                    SetError(error, "Skin manifest has an invalid image tint");
                    return std::nullopt;
                }
                image.tint = *parsed;
            }
            if (const auto overPanels = get("over_panels")) {
                const auto parsed = ParseBool(*overPanels);
                if (!parsed) {
                    SetError(error, "Skin manifest has an invalid image over_panels");
                    return std::nullopt;
                }
                image.overPanels = *parsed;
            }
            if (const auto overScreens = get("over_screens")) {
                const auto parsed = ParseBool(*overScreens);
                if (!parsed) {
                    SetError(error, "Skin manifest has an invalid image over_screens");
                    return std::nullopt;
                }
                image.overScreens = *parsed;
            }
            if (const auto priority = get("priority")) {
                const auto parsed = ParseSize(*priority);
                if (!parsed || *parsed < 1 || *parsed > 99) {
                    SetError(error, "Skin manifest has an invalid image priority");
                    return std::nullopt;
                }
                image.priority = static_cast<std::uint8_t>(*parsed);
            }
        }
    }

    if (!Validate(result, error)) {
        return std::nullopt;
    }
    if (error != nullptr) {
        error->clear();
    }
    return result;
}

bool Skin::SaveManifestAtomic(
    const std::filesystem::path& manifestPath,
    std::string* error) const {
    if (!Validate(*this, error)) {
        return false;
    }

    core::IniDocument document;
    document.Set("skin", "format", "1");
    document.Set("skin", "id", id);
    document.Set("skin", "name", name);
    document.Set("skin", "author", author);
    document.Set("skin", "version", version);
    for (const auto& field : kPaletteFields) {
        document.Set("colors", std::string(field.key), FormatColor(colors.*(field.member)));
    }
    document.Set("font", "family", typography.fontFamily);
    document.Set("font", "custom_file",
                 core::PathToUtf8(typography.customFontFile).value_or(std::string{}));
    document.Set("font", "base_size", FormatFloat(typography.baseSize));
    document.Set("font", "border_size", FormatFloat(typography.borderSize));
    document.Set("appearance", "transparent_buttons", BoolText(appearance.transparentButtons));
    document.Set("appearance", "show_title_bars", BoolText(appearance.showTitleBars));
    document.Set("appearance", "show_panel_borders", BoolText(appearance.showPanelBorders));
    document.Set("appearance", "decor_above_panels", BoolText(appearance.decorAbovePanels));
    document.Set("appearance", "background_image",
                 core::PathToUtf8(appearance.backgroundImage).value_or(std::string{}));
    document.Set("appearance", "background_image_opacity", FormatFloat(appearance.backgroundImageOpacity));
    document.Set("appearance", "panel_opacity", FormatFloat(appearance.panelOpacity));
    document.Set("appearance", "screen_opacity", FormatFloat(appearance.screenOpacity));
    document.Set("appearance", "centered_titles", BoolText(appearance.centeredTitles));
    document.Set("shapes", "count", std::to_string(shapes.size()));
    for (std::size_t index = 0; index < shapes.size(); ++index) {
        const std::string prefix = "shape" + std::to_string(index) + ".";
        const auto& shape = shapes[index];
        document.Set("shapes", prefix + "kind", std::string(ShapeKindText(shape.kind)));
        document.Set("shapes", prefix + "x", FormatFloat(shape.x));
        document.Set("shapes", prefix + "y", FormatFloat(shape.y));
        document.Set("shapes", prefix + "width", FormatFloat(shape.width));
        document.Set("shapes", prefix + "height", FormatFloat(shape.height));
        document.Set("shapes", prefix + "filled", BoolText(shape.filled));
        document.Set("shapes", prefix + "color", FormatColor(shape.color));
        document.Set("shapes", prefix + "stroke_width", FormatFloat(shape.strokeWidth));
        document.Set("shapes", prefix + "rotation", FormatFloat(shape.rotation));
        document.Set("shapes", prefix + "opacity", FormatFloat(shape.opacity));
        document.Set("shapes", prefix + "flip_horizontal", BoolText(shape.flipHorizontal));
        document.Set("shapes", prefix + "flip_vertical", BoolText(shape.flipVertical));
        document.Set("shapes", prefix + "over_panels", BoolText(shape.overPanels));
        document.Set("shapes", prefix + "over_screens", BoolText(shape.overScreens));
        document.Set("shapes", prefix + "priority", std::to_string(shape.priority));
    }
    document.Set("images", "count", std::to_string(images.size()));
    for (std::size_t index = 0; index < images.size(); ++index) {
        const std::string prefix = "image" + std::to_string(index) + ".";
        const auto& image = images[index];
        document.Set("images", prefix + "file",
                     core::PathToUtf8(image.file).value_or(std::string{}));
        document.Set("images", prefix + "x", FormatFloat(image.x));
        document.Set("images", prefix + "y", FormatFloat(image.y));
        document.Set("images", prefix + "width", FormatFloat(image.width));
        document.Set("images", prefix + "height", FormatFloat(image.height));
        document.Set("images", prefix + "opacity", FormatFloat(image.opacity));
        document.Set("images", prefix + "rotation", FormatFloat(image.rotation));
        document.Set("images", prefix + "flip_horizontal", BoolText(image.flipHorizontal));
        document.Set("images", prefix + "flip_vertical", BoolText(image.flipVertical));
        document.Set("images", prefix + "over_panels", BoolText(image.overPanels));
        document.Set("images", prefix + "over_screens", BoolText(image.overScreens));
        document.Set("images", prefix + "priority", std::to_string(image.priority));
        document.Set("images", prefix + "tint", FormatColor(image.tint));
    }
    return document.SaveAtomic(manifestPath, error);
}

bool Skin::Validate(const Skin& skin, std::string* error) {
    if (!IsId(skin.id)) {
        SetError(error, "Skin id must contain 1-64 lowercase ASCII letters, digits, '-' or '_'");
        return false;
    }
    if (!IsMetadata(skin.name, kMaximumNameBytes)) {
        SetError(error, "Skin name must be trimmed UTF-8 text up to 128 bytes");
        return false;
    }
    if (!IsMetadata(skin.author, kMaximumAuthorBytes)) {
        SetError(error, "Skin author must be trimmed UTF-8 text up to 128 bytes");
        return false;
    }
    if (!IsMetadata(skin.version, kMaximumVersionBytes)) {
        SetError(error, "Skin version must be trimmed UTF-8 text up to 32 bytes");
        return false;
    }
    if (!IsMetadata(skin.typography.fontFamily, kMaximumFontBytes)) {
        SetError(error, "Skin font family must be trimmed UTF-8 text up to 128 bytes");
        return false;
    }
    if (skin.typography.baseSize < kMinimumFontSize || skin.typography.baseSize > kMaximumFontSize ||
        !std::isfinite(skin.typography.baseSize)) {
        SetError(error, "Skin font size must be between 8 and 32 points");
        return false;
    }
    if (!std::isfinite(skin.typography.borderSize) || skin.typography.borderSize < 0.0F ||
        skin.typography.borderSize > 8.0F) {
        SetError(error, "Skin font border size must be between 0 and 8 pixels");
        return false;
    }
    if (!IsSafeAssetPath(skin.typography.customFontFile) ||
        !IsSafeAssetPath(skin.appearance.backgroundImage)) {
        SetError(error, "Skin asset paths must be relative files inside the skin directory");
        return false;
    }
    if (!std::isfinite(skin.appearance.backgroundImageOpacity) ||
        skin.appearance.backgroundImageOpacity < 0.0F || skin.appearance.backgroundImageOpacity > 1.0F ||
        !std::isfinite(skin.appearance.panelOpacity) ||
        skin.appearance.panelOpacity < 0.0F || skin.appearance.panelOpacity > 1.0F ||
        !std::isfinite(skin.appearance.screenOpacity) ||
        skin.appearance.screenOpacity < 0.0F || skin.appearance.screenOpacity > 1.0F) {
        SetError(error, "Skin opacity values must be between 0 and 1");
        return false;
    }
    if (skin.shapes.size() > kMaximumShapes) {
        SetError(error, "Skin may contain at most 64 shapes");
        return false;
    }
    for (const auto& shape : skin.shapes) {
        if (!std::isfinite(shape.x) || !std::isfinite(shape.y) || !std::isfinite(shape.rotation) ||
             !std::isfinite(shape.width) || !std::isfinite(shape.height) ||
             !std::isfinite(shape.strokeWidth) || !std::isfinite(shape.opacity) ||
            shape.x < -1.0F || shape.x > 2.0F || shape.y < -1.0F || shape.y > 2.0F ||
             shape.width < 0.0F || shape.width > 2.0F ||
             shape.height < 0.0F || shape.height > 2.0F ||
             shape.strokeWidth < 0.0F || shape.strokeWidth > 32.0F ||
             shape.opacity < 0.0F || shape.opacity > 1.0F ||
             shape.priority < 1 || shape.priority > 99) {
            SetError(error, "Skin shape geometry is outside supported normalized bounds");
            return false;
        }
    }
    if (skin.images.size() > kMaximumImages) {
        SetError(error, "Skin may contain at most 32 images");
        return false;
    }
    for (const auto& image : skin.images) {
        if (image.file.empty() || !IsSafeAssetPath(image.file)) {
            SetError(error, "Skin image paths must be relative files inside the skin directory");
            return false;
        }
        if (!std::isfinite(image.x) || !std::isfinite(image.y) ||
            !std::isfinite(image.width) || !std::isfinite(image.height) ||
            !std::isfinite(image.opacity) || !std::isfinite(image.rotation) ||
            image.x < -1.0F || image.x > 2.0F || image.y < -1.0F || image.y > 2.0F ||
            image.width <= 0.0F || image.width > 2.0F ||
            image.height <= 0.0F || image.height > 2.0F ||
            image.opacity < 0.0F || image.opacity > 1.0F ||
             image.rotation < -360.0F || image.rotation > 360.0F ||
             image.priority < 1 || image.priority > 99) {
            SetError(error, "Skin image geometry is outside supported normalized bounds");
            return false;
        }
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::optional<Color> ParseColor(std::string_view value) noexcept {
    if ((value.size() != 7 && value.size() != 9) || value.front() != '#') {
        return std::nullopt;
    }
    const auto red = HexByte(value[1], value[2]);
    const auto green = HexByte(value[3], value[4]);
    const auto blue = HexByte(value[5], value[6]);
    const auto alpha = value.size() == 9 ? HexByte(value[7], value[8])
                                        : std::optional<std::uint8_t>{static_cast<std::uint8_t>(255)};
    if (!red || !green || !blue || !alpha) {
        return std::nullopt;
    }
    return Color{*red, *green, *blue, *alpha};
}

std::string FormatColor(Color color) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::array values{color.red, color.green, color.blue, color.alpha};
    std::string output = "#";
    output.reserve(9);
    for (const auto value : values) {
        output.push_back(hex[value >> 4U]);
        output.push_back(hex[value & 0x0FU]);
    }
    return output;
}

} // namespace rivan::skin
