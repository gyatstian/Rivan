// Rivan source file
// Purpose: Skin discovery, fallback resolution, and folder installation.
#include "SkinManager.h"

#include "SkinArchive.h"

#include "../core/AppPaths.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

namespace rivan::skin {
namespace {

constexpr std::uintmax_t kMaximumPackageBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumPackageFiles = 512;
std::atomic_uint64_t g_installSequence{0};

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

void AddWarning(std::string* warnings, std::string message) {
    if (warnings == nullptr) {
        return;
    }
    if (!warnings->empty()) {
        warnings->push_back('\n');
    }
    *warnings += std::move(message);
}

std::string PathDisplay(const std::filesystem::path& path) {
    const std::wstring text = path.wstring();
    if (text.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                              static_cast<int>(text.size()), nullptr, 0,
                                              nullptr, nullptr);
    if (required <= 0) {
        return "<path>";
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), required, nullptr, nullptr);
    return result;
}

std::filesystem::path UniqueSibling(const std::filesystem::path& parent,
                                    std::wstring_view prefix) {
    const auto value = g_installSequence.fetch_add(1, std::memory_order_relaxed);
    return parent / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) +
                     L"." + std::to_wstring(value));
}

bool ContainsReparsePoint(const std::filesystem::path& source,
                          const std::filesystem::directory_entry& entry,
                          std::string* error) {
    const DWORD attributes = GetFileAttributesW(entry.path().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        SetError(error, "Unable to inspect skin package entry: " + PathDisplay(entry.path()));
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        SetError(error, "Skin package cannot contain reparse points: " +
                        PathDisplay(std::filesystem::relative(entry.path(), source)));
        return true;
    }
    return false;
}

} // namespace

SkinManager::SkinManager()
    : SkinManager(core::AppPaths::SkinsDirectory()) {}

SkinManager::SkinManager(std::filesystem::path skinsDirectory)
    : skinsDirectory_(std::move(skinsDirectory)),
      workingDirectory_(core::AppPaths::LocalDataRoot() / L"skin-cache"),
      skins_{Skin::BuiltInDarkPurple()} {}

bool SkinManager::Refresh(std::string* error, std::string* warnings) {
    if (warnings != nullptr) {
        warnings->clear();
    }
    skins_.assign(1, Skin::BuiltInDarkPurple());

    std::error_code ec;
    std::filesystem::create_directories(skinsDirectory_, ec);
    if (ec) {
        SetError(error, "Unable to create skins directory: " + ec.message());
        return false;
    }

    std::filesystem::remove_all(workingDirectory_, ec);
    ec.clear();
    std::filesystem::create_directories(workingDirectory_, ec);
    if (ec) {
        SetError(error, "Unable to create skin cache directory: " + ec.message());
        return false;
    }
    std::vector<std::filesystem::path> packages;
    std::vector<std::filesystem::path> folders;
    std::filesystem::directory_iterator iterator(skinsDirectory_, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        SetError(error, "Unable to enumerate skins directory: " + ec.message());
        return false;
    }
    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            SetError(error, "Unable to enumerate skins directory: " + ec.message());
            return false;
        }
        if (iterator->is_regular_file(ec) && !ec && iterator->path().extension() == L".rivanskin") {
            packages.push_back(iterator->path());
        } else if (iterator->is_directory(ec) && !ec) {
            const auto manifest = iterator->path() / Skin::ManifestFileName;
            if (std::filesystem::is_regular_file(manifest, ec) && !ec) {
                folders.push_back(iterator->path());
            }
        }
        if (ec) {
            ec.clear();
        }
    }
    std::sort(packages.begin(), packages.end(), [](const auto& left, const auto& right) {
        return left.native() < right.native();
    });

    for (const auto& package : packages) {
        std::string manifestError;
        const auto extraction = workingDirectory_ / package.stem();
        if (!ExtractSkinArchive(package, extraction, &manifestError)) {
            AddWarning(warnings, "Skipping " + PathDisplay(package) + ": " + manifestError);
            continue;
        }
        auto candidate = Skin::LoadManifest(extraction / Skin::ManifestFileName, &manifestError);
        if (!candidate) {
            AddWarning(warnings, "Skipping " + PathDisplay(package) + ": " + manifestError);
            continue;
        }
        if (candidate->id == Skin::BuiltInId) {
            // A materialized default is documentation/editable seed data; the compiled
            // fallback remains authoritative and cannot be shadowed.
            continue;
        }
        if (Find(candidate->id) != nullptr) {
            AddWarning(warnings, "Skipping duplicate skin id '" + candidate->id + "'");
            continue;
        }
        candidate->directory = extraction;
        skins_.push_back(std::move(*candidate));
    }
    std::sort(folders.begin(), folders.end(), [](const auto& left, const auto& right) {
        return left.native() < right.native();
    });
    for (const auto& folder : folders) {
        std::string manifestError;
        auto candidate = Skin::LoadManifest(folder / Skin::ManifestFileName, &manifestError);
        if (!candidate) {
            AddWarning(warnings, "Skipping " + PathDisplay(folder) + ": " + manifestError);
            continue;
        }
        if (candidate->id == Skin::BuiltInId) continue;
        if (Find(candidate->id) != nullptr) {
            AddWarning(warnings, "Skipping folder skin with duplicate id '" + candidate->id + "'");
            continue;
        }
        candidate->directory = folder;
        skins_.push_back(std::move(*candidate));
    }

    std::sort(skins_.begin() + 1, skins_.end(), [](const Skin& left, const Skin& right) {
        return left.name < right.name || (left.name == right.name && left.id < right.id);
    });
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

const Skin* SkinManager::Find(std::string_view id) const noexcept {
    const auto found = std::find_if(skins_.begin(), skins_.end(), [id](const Skin& skin) {
        return skin.id == id;
    });
    return found == skins_.end() ? nullptr : &*found;
}

const Skin& SkinManager::Resolve(std::string_view id) const noexcept {
    if (const auto* found = Find(id)) {
        return *found;
    }
    return Fallback();
}

const Skin& SkinManager::Fallback() const noexcept {
    // Construction and Refresh both preserve the fallback at index zero.
    return skins_.front();
}

bool SkinManager::SavePackage(const Skin& skin, std::string* error) const {
    if (skin.builtIn || skin.id.empty() || skin.id == Skin::BuiltInId) {
        SetError(error, "The built-in skin cannot be saved as a package");
        return false;
    }
    const auto sourceDirectory = skin.directory;
    std::error_code ec;
    if (!std::filesystem::is_directory(sourceDirectory, ec) || ec) {
        SetError(error, "Skin working directory is unavailable");
        return false;
    }
    std::filesystem::create_directories(skinsDirectory_, ec);
    if (ec) {
        SetError(error, "Unable to create skins directory: " + ec.message());
        return false;
    }

    const auto staging = UniqueSibling(skinsDirectory_, L".package-");
    if (!CreateSkinArchive(sourceDirectory, staging, error)) return false;
    const auto destination = PackagePath(skin.id);
    std::filesystem::remove(destination, ec);
    std::filesystem::rename(staging, destination, ec);
    if (ec) { std::filesystem::remove(staging, ec); SetError(error, "Unable to save skin package: " + ec.message()); return false; }
    return true;
}

std::filesystem::path SkinManager::WorkingDirectory(std::string_view id) const {
    return workingDirectory_ / std::filesystem::path(std::string(id));
}

std::filesystem::path SkinManager::PackagePath(std::string_view id) const {
    return skinsDirectory_ / (std::filesystem::path(std::string(id)) += L".rivanskin");
}

} // namespace rivan::skin
