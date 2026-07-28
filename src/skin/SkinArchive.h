// Rivan source file
// Purpose: Safe ZIP packaging used by .rivanskin files.
#pragma once

#include <filesystem>
#include <string>

namespace rivan::skin {

[[nodiscard]] bool ExtractSkinArchive(const std::filesystem::path& archive,
                                      const std::filesystem::path& destination,
                                      std::string* error = nullptr);
[[nodiscard]] bool CreateSkinArchive(const std::filesystem::path& source,
                                     const std::filesystem::path& archive,
                                     std::string* error = nullptr);

} // namespace rivan::skin
