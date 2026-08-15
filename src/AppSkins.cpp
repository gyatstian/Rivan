// AppSkins.cpp
// Skin selection, studio editing, package persistence, and asset import.
#include "App.h"

#include "core/FileSystemUtil.h"
#include "core/Text.h"

#include <shellapi.h>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace rivan {
namespace {

std::string Narrow(std::string_view text) {
    return std::string(text);
}

// Skin ids are validated to be lowercase ASCII (letters, digits, '-', '_'), so a
// direct narrowing is sufficient and any non-ASCII input simply fails Find() later.
std::string Narrow(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t ch : text) {
        result.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return result;
}

std::string MakeEditableSkinId(std::string id) {
    if (id.empty() || id == std::string(skin::Skin::BuiltInId)) return "custom-skin";
    return id;
}

std::string SkinIdFromName(std::wstring_view name) {
    std::string id;
    bool separator = false;
    for (const wchar_t character : name) {
        if (character < 128 && std::isalnum(static_cast<unsigned char>(character))) {
            if (separator && !id.empty()) id.push_back('-');
            id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
            separator = false;
        } else {
            separator = true;
        }
        if (id.size() >= 56) break;
    }
    return id.empty() ? "custom-skin" : id;
}

std::filesystem::path AssetFolderFor(ui::SkinAssetKind kind) {
    return kind == ui::SkinAssetKind::Font ? std::filesystem::path(L"fonts")
                                           : std::filesystem::path(L"images");
}

bool IsSupportedAssetExtension(const std::filesystem::path& path, ui::SkinAssetKind kind) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    if (kind == ui::SkinAssetKind::Font) {
        return ext == L".ttf" || ext == L".otf" || ext == L".ttc";
    }
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".webp";
}

std::filesystem::path SafeFileName(const std::filesystem::path& source) {
    auto name = source.filename().wstring();
    if (name.empty()) name = L"asset" + source.extension().wstring();
    for (auto& ch : name) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' ||
            ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    return std::filesystem::path(name);
}

} // namespace

void App::ApplySkin(std::wstring_view id) {
    const std::string narrowId = Narrow(id);
    if (skins_.Find(narrowId) == nullptr) return;
    committedSkin_ = skins_.Resolve(narrowId);
    activeSkin_ = committedSkin_;
    auto settings = settings_.Settings();
    settings.skinId = committedSkin_.id;
    std::string ignored;
    (void)settings_.SetSettings(settings, &ignored);
    (void)settings_.SaveSettings(&ignored);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::EditSkin(std::wstring_view id) {
    const auto* selected = skins_.Find(Narrow(id));
    if (selected == nullptr || selected->builtIn) return;
    committedSkin_ = *selected;
    activeSkin_ = committedSkin_;
    skinStudioVisible_ = true;
    skinStudioEditExisting_ = true;
    settingsVisible_ = false;
    ++revision_;
    if (window_) window_->Refresh();
}

bool App::RenameSkin(std::wstring_view id, std::wstring_view name, std::wstring& error) {
    const auto* selected = skins_.Find(Narrow(id));
    if (selected == nullptr || selected->builtIn) {
        error = L"Only saved skins can be renamed.";
        return false;
    }
    skin::Skin renamed = *selected;
    const std::string previousId = renamed.id;
    renamed.name = Narrow(name);

    // Filename (and manifest id) is derived from display name so package on disk tracks it.
    std::string newId = SkinIdFromName(name);
    if (newId != previousId) {
        const std::string base = newId;
        for (int suffix = 2; skins_.Find(newId) != nullptr; ++suffix) {
            newId = base + "-" + std::to_string(suffix);
        }
    }
    renamed.id = newId;

    std::string narrowError;
    if (!renamed.SaveManifestAtomic(renamed.directory / skin::Skin::ManifestFileName, &narrowError) ||
        !skins_.SavePackage(renamed, &narrowError)) {
        error = core::Utf8ToWide(narrowError, L"Unable to decode error text");
        return false;
    }
    if (newId != previousId) {
        std::error_code ec;
        std::filesystem::remove(skins_.PackagePath(previousId), ec);
    }
    if (!skins_.Refresh(&narrowError, nullptr)) {
        error = core::Utf8ToWide(narrowError, L"Unable to decode error text");
        return false;
    }
    if (committedSkin_.id == previousId) {
        committedSkin_ = skins_.Resolve(newId);
        activeSkin_ = committedSkin_;
        auto settings = settings_.Settings();
        settings.skinId = committedSkin_.id;
        (void)settings_.SetSettings(settings, &narrowError);
        (void)settings_.SaveSettings(&narrowError);
    }
    ++revision_;
    if (window_) window_->Refresh();
    error.clear();
    return true;
}

bool App::DeleteSkin(std::wstring_view id, std::wstring& error) {
    const auto* selected = skins_.Find(Narrow(id));
    if (selected == nullptr || selected->builtIn) {
        error = L"Only saved skins can be deleted.";
        return false;
    }
    const std::string deletedId = selected->id;
    std::error_code ec;
    // Folder skins (a directory inside the skins directory with a skin.ini) have no
    // package file; remove the directory. Package skins delete the .rivanskin file.
    const auto package = skins_.PackagePath(selected->id);
    const bool isFolderSkin = selected->directory.parent_path() == skins_.SkinsDirectory();
    if (isFolderSkin) {
        std::filesystem::remove_all(selected->directory, ec);
    } else {
        std::filesystem::remove(package, ec);
    }
    if (ec) {
        error = L"Unable to delete skin: " + std::filesystem::path(ec.message()).wstring();
        return false;
    }
    // Verify the skin is really gone; folder skin manifests may live in a nested
    // working directory, in which case removing the folder was still the right call.
    std::string narrowError;
    if (!skins_.Refresh(&narrowError, nullptr)) {
        error = core::Utf8ToWide(narrowError, L"Unable to decode error text");
        return false;
    }
    if (skins_.Find(deletedId) != nullptr) {
        error = L"Unable to delete skin: the skin is still present after removal.";
        return false;
    }
    if (committedSkin_.id == deletedId) {
        committedSkin_ = skins_.Fallback();
        activeSkin_ = committedSkin_;
        auto settings = settings_.Settings();
        settings.skinId = committedSkin_.id;
        (void)settings_.SetSettings(settings, &narrowError);
        (void)settings_.SaveSettings(&narrowError);
    }
    ++revision_;
    if (window_) window_->Refresh();
    error.clear();
    return true;
}

void App::FocusSkinColor(std::size_t index) {
    focusedSkinColor_ = index;
    ++skinColorFocusRevision_;
    ++revision_;
    if (window_) window_->Refresh();
}

void App::FocusSkinElement(int element) {
    focusedSkinElement_ = element;
    ++skinElementFocusRevision_;
    ++revision_;
    if (window_) window_->Refresh();
}

void App::OpenSkinFolder() {
    std::error_code ec;
    std::filesystem::create_directories(skins_.SkinsDirectory(), ec);
    ShellExecuteW(nullptr, L"open", skins_.SkinsDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::PreviewSkin(skin::Skin candidate) {
    if ((candidate.builtIn || candidate.id.empty() || candidate.id == skin::Skin::BuiltInId) &&
        (!candidate.typography.customFontFile.empty() || !candidate.images.empty())) {
        candidate.directory = skins_.WorkingDirectory(".studio-assets");
    }
    activeSkin_ = std::move(candidate);
    ++revision_;
    if (window_) window_->Refresh();
}

bool App::SaveSkin(skin::Skin candidate, std::wstring& error) {
    const auto sourceDirectory = candidate.directory;
    std::filesystem::path stagedAssets;
    const bool createNew = candidate.builtIn || candidate.id.empty();
    if (createNew) {
        stagedAssets = skins_.WorkingDirectory(".studio-assets");
        const std::wstring name = core::Utf8ToWide(candidate.name, L"Unable to decode error text");
        const std::string base = SkinIdFromName(name);
        candidate.id = base;
        for (int suffix = 2; skins_.Find(candidate.id) != nullptr; ++suffix) {
            candidate.id = base + "-" + std::to_string(suffix);
        }
    } else {
        candidate.id = MakeEditableSkinId(std::move(candidate.id));
    }
    candidate.builtIn = false;
    candidate.directory = skins_.WorkingDirectory(candidate.id);
    std::string narrowError;
    if (!skin::Skin::Validate(candidate, &narrowError)) {
        error = core::Utf8ToWide(narrowError, L"Unable to decode error text");
        return false;
    }
    std::error_code ec;
    if (!stagedAssets.empty() && stagedAssets != candidate.directory &&
        std::filesystem::exists(stagedAssets, ec) && !ec) {
        std::filesystem::rename(stagedAssets, candidate.directory, ec);
        if (ec) {
            error = L"Unable to activate imported skin assets: " +
                    std::filesystem::path(ec.message()).wstring();
            return false;
        }
    }
    ec.clear();
    std::filesystem::create_directories(candidate.directory, ec);
    if (ec) {
        error = L"Unable to create skin directory: " + std::filesystem::path(ec.message()).wstring();
        return false;
    }
    // Forking a skin copies images/fonts from source; only .studio-assets is renamed above.
    if (createNew && !sourceDirectory.empty() && sourceDirectory != candidate.directory &&
        std::filesystem::exists(sourceDirectory, ec) && !ec) {
        for (const auto* folder : {L"images", L"fonts"}) {
            const auto from = sourceDirectory / folder;
            if (!std::filesystem::exists(from, ec) || ec) continue;
            const auto to = candidate.directory / folder;
            std::filesystem::create_directories(to, ec);
            for (std::filesystem::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || ec) continue;
                const auto dest = to / it->path().filename();
                if (std::filesystem::exists(dest, ec) && !ec) continue;
                std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::none, ec);
                if (ec) {
                    error = L"Unable to copy skin asset: " +
                            std::filesystem::path(ec.message()).wstring();
                    return false;
                }
            }
        }
    }
    if (!candidate.SaveManifestAtomic(candidate.directory / skin::Skin::ManifestFileName, &narrowError) ||
        !skins_.SavePackage(candidate, &narrowError)) {
        error = core::Utf8ToWide(narrowError, L"Unable to decode error text");
        return false;
    }
    std::string warnings;
    if (!skins_.Refresh(&narrowError, &warnings)) {
        error = core::Utf8ToWide(narrowError, L"Unable to decode error text");
        return false;
    }
    auto settings = settings_.Settings();
    settings.skinId = candidate.id;
    if (!settings_.SetSettings(settings, &narrowError) || !settings_.SaveSettings(&narrowError)) {
        error = core::Utf8ToWide(narrowError, L"Unable to decode error text");
        return false;
    }
    committedSkin_ = skins_.Resolve(candidate.id);
    activeSkin_ = committedSkin_;
    ++revision_;
    if (window_) window_->Refresh();
    error.clear();
    return true;
}

void App::CancelSkinPreview() {
    activeSkin_ = committedSkin_;
    ++revision_;
    if (window_) window_->Refresh();
}

std::optional<std::filesystem::path> App::ImportSkinAsset(
    std::string_view skinId,
    const std::filesystem::path& source,
    ui::SkinAssetKind kind,
    std::wstring& error) {
    if (!IsSupportedAssetExtension(source, kind)) {
        error = kind == ui::SkinAssetKind::Font
            ? L"Choose a .ttf, .otf, or .ttc font file."
            : L"Choose a .png, .jpg, .jpeg, .bmp, or .webp image file.";
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec) {
        error = L"Skin asset source is not a readable file.";
        return std::nullopt;
    }
    const std::string rawId = Narrow(skinId);
    const std::filesystem::path id = rawId.empty() || rawId == skin::Skin::BuiltInId
        ? skins_.WorkingDirectory(".studio-assets")
        : skins_.WorkingDirectory(MakeEditableSkinId(rawId));
    const auto folder = AssetFolderFor(kind);
    const auto directory = id / folder;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = L"Unable to create skin asset directory: " + std::filesystem::path(ec.message()).wstring();
        return std::nullopt;
    }
    const auto destination = core::UniqueDestination(directory, SafeFileName(source));
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec);
    if (ec) {
        error = L"Unable to copy skin asset: " + std::filesystem::path(ec.message()).wstring();
        return std::nullopt;
    }
    error.clear();
    return folder / destination.filename();
}

} // namespace rivan
