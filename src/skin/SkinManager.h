// Rivan source file
// Purpose: Skin discovery, fallback resolution, and .rivanskin storage.
#pragma once

#include "Skin.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rivan::skin {

class SkinManager final {
public:
    SkinManager();
    explicit SkinManager(std::filesystem::path skinsDirectory);

    // Refresh always retains the built-in dark-purple skin. Invalid manifests are
    // skipped and reported as newline-separated warnings.
    [[nodiscard]] bool Refresh(std::string* error = nullptr, std::string* warnings = nullptr);

    [[nodiscard]] const std::vector<Skin>& Skins() const noexcept { return skins_; }
    [[nodiscard]] const Skin* Find(std::string_view id) const noexcept;
    [[nodiscard]] const Skin& Resolve(std::string_view id) const noexcept;
    [[nodiscard]] const Skin& Fallback() const noexcept;
    [[nodiscard]] const std::filesystem::path& SkinsDirectory() const noexcept {
        return skinsDirectory_;
    }

    // Commits an extracted working skin as <skins>/<id>.rivanskin.
    [[nodiscard]] bool SavePackage(const Skin& skin, std::string* error = nullptr) const;
    [[nodiscard]] std::filesystem::path WorkingDirectory(std::string_view id) const;
    [[nodiscard]] std::filesystem::path PackagePath(std::string_view id) const;

private:
    std::filesystem::path skinsDirectory_;
    std::filesystem::path workingDirectory_;
    std::vector<Skin> skins_;
};

} // namespace rivan::skin
