// Rivan source file
// Purpose: Compact UTF-8 INI parsing and atomic persistence.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace rivan::core {

class IniDocument final {
public:
    using Section = std::map<std::string, std::string, std::less<>>;
    using Sections = std::map<std::string, Section, std::less<>>;

    [[nodiscard]] static std::optional<IniDocument> Parse(
        std::string_view utf8,
        std::string* error = nullptr);
    [[nodiscard]] static std::optional<IniDocument> Load(
        const std::filesystem::path& path,
        std::string* error = nullptr);

    [[nodiscard]] std::string Serialize() const;
    [[nodiscard]] bool SaveAtomic(
        const std::filesystem::path& path,
        std::string* error = nullptr) const;

    [[nodiscard]] std::optional<std::string_view> Get(
        std::string_view section,
        std::string_view key) const noexcept;

    [[nodiscard]] bool HasMetaFormat(std::string_view expected) const noexcept;

    void Set(std::string section, std::string key, std::string value);

    [[nodiscard]] const Sections& Data() const noexcept { return sections_; }

private:
    Sections sections_;
};

} // namespace rivan::core
