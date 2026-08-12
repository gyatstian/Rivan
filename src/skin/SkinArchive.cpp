// Rivan source file
// Purpose: Safe ZIP packaging used by .rivanskin files.
#include "SkinArchive.h"

#ifdef _WIN32
#include "../core/Text.h"
#endif
#include "miniz/miniz.h"

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace rivan::skin {
namespace {

constexpr std::uint64_t kMaximumPackageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumPackageFiles = 512;
constexpr std::uint64_t kMaximumPackageArchiveBytes = 256ULL * 1024ULL * 1024ULL;

void SetError(std::string* error, std::string value) {
    if (error != nullptr) *error = std::move(value);
}

std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.wstring();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

std::optional<std::filesystem::path> DecodeArchivePath(const char* name) {
    if (name == nullptr || *name == '\0') return std::nullopt;
    std::filesystem::path path;
#ifdef _WIN32
    // ZIP names are UTF-8. Constructing a Windows path from char* uses the ANSI
    // code page and corrupts non-ASCII entry names.
    const auto wideName = core::Utf8ToWide(name);
    if (wideName.empty()) return std::nullopt;
    path = std::filesystem::path(wideName);
#else
    path = std::filesystem::u8path(name);
#endif
    return path;
}

bool SafeArchivePath(const char* name, std::filesystem::path* result) {
    const auto decoded = DecodeArchivePath(name);
    if (!decoded) return false;
    auto path = *decoded;
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        auto trimmed = part.native();
        while (!trimmed.empty() && (trimmed.back() == L'.' || trimmed.back() == L' ')) {
            trimmed.pop_back();
        }
        // Win32 strips trailing dots/spaces from path components, so ".. " and
        // "foo. " normalize differently after extraction; reject them outright.
        if (part.empty() || part == L"." || part == L".." || trimmed != part.native() ||
            trimmed.empty() || trimmed == L"." || trimmed == L"..") {
            return false;
        }
    }
    *result = std::move(path);
    return true;
}

} // namespace

bool ExtractSkinArchive(const std::filesystem::path& archive,
                        const std::filesystem::path& destination,
                        std::string* error) {
    std::error_code ec;
    const auto archiveSize = std::filesystem::file_size(archive, ec);
    MZ_FILE* package = nullptr;
    (void)_wfopen_s(&package, archive.wstring().c_str(), L"rb");
    if (package == nullptr) {
        SetError(error, "Unable to open skin package");
        return false;
    }
    if (ec) {
        // file_size failed (file may have been opened above); release the handle.
        fclose(package);
        SetError(error, "Unable to open skin package");
        return false;
    }
    if (archiveSize > kMaximumPackageArchiveBytes) {
        fclose(package);
        SetError(error, "Skin package is too large");
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, package, archiveSize, 0)) {
        fclose(package);
        SetError(error, "Unable to open skin package");
        return false;
    }
    const auto close = [&] {
        mz_zip_reader_end(&zip);
        fclose(package);
    };
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > kMaximumPackageFiles) {
        close();
        SetError(error, "Skin package exceeds the 512-file limit");
        return false;
    }
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        close();
        SetError(error, "Unable to create skin cache directory: " + ec.message());
        return false;
    }
    std::uint64_t total = 0;
    const auto normalizedDestination = std::filesystem::path(destination).lexically_normal();
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat entry{};
        std::filesystem::path relative;
        if (!mz_zip_reader_file_stat(&zip, index, &entry) || !SafeArchivePath(entry.m_filename, &relative)) {
            close();
            SetError(error, "Skin package contains an unsafe path");
            return false;
        }
        const auto target = destination / relative;
        // Defense in depth against zip-slip: normalize the relative path and
        // confirm the produced target still resolves underneath the destination.
        // lexically_normal must be used (the target file may not exist yet, so
        // weakly_canonical would be wrong here).
        const auto normalizedTarget = (normalizedDestination / relative.lexically_normal()).lexically_normal();
        if (normalizedTarget.lexically_relative(normalizedDestination).empty()) {
            close();
            SetError(error, "Skin package contains an unsafe path");
            return false;
        }
        if (entry.m_is_directory) {
            std::filesystem::create_directories(target, ec);
            if (ec) { close(); SetError(error, "Unable to unpack skin directory: " + ec.message()); return false; }
            continue;
        }
        if (entry.m_uncomp_size > kMaximumPackageBytes - total) {
            close();
            SetError(error, "Skin package exceeds the 64 MiB limit");
            return false;
        }
        total += entry.m_uncomp_size;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            close();
            SetError(error, "Unable to unpack skin file");
            return false;
        }
        MZ_FILE* out = nullptr;
        (void)_wfopen_s(&out, target.wstring().c_str(), L"wb");
        if (out == nullptr) {
            close();
            SetError(error, "Unable to unpack skin file");
            return false;
        }
        const bool ok = mz_zip_reader_extract_to_cfile(&zip, index, out, 0) != FALSE;
        fclose(out);
        if (!ok) {
            close();
            SetError(error, "Unable to unpack skin file");
            return false;
        }
    }
    close();
    if (error != nullptr) error->clear();
    return true;
}

bool CreateSkinArchive(const std::filesystem::path& source,
                       const std::filesystem::path& archive,
                       std::string* error) {
    MZ_FILE* package = nullptr;
    (void)_wfopen_s(&package, archive.wstring().c_str(), L"wb");
    if (package == nullptr) {
        SetError(error, "Unable to create skin package");
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_cfile(&zip, package, 0)) {
        fclose(package);
        SetError(error, "Unable to create skin package");
        return false;
    }
    std::error_code ec;
    std::uint64_t total = 0;
    std::size_t count = 0;
    const auto abortArchive = [&] {
        mz_zip_writer_end(&zip);
        fclose(package);
        std::filesystem::remove(archive, ec);
    };
    for (std::filesystem::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        const auto size = it->file_size(ec);
        if (ec || ++count > kMaximumPackageFiles || size > kMaximumPackageBytes - total) {
            abortArchive();
            SetError(error, "Skin package exceeds the 512-file or 64 MiB limit");
            return false;
        }
        total += size;
        const auto relative = std::filesystem::relative(it->path(), source, ec);
        const auto entryName = Utf8(relative.generic_wstring());
        MZ_FILE* in = nullptr;
        (void)_wfopen_s(&in, it->path().wstring().c_str(), L"rb");
        if (ec || in == nullptr) {
            if (in != nullptr) fclose(in);
            abortArchive();
            SetError(error, "Unable to add file to skin package");
            return false;
        }
        const bool added = mz_zip_writer_add_cfile(
            &zip, entryName.c_str(), in, size, nullptr, nullptr, 0,
            MZ_BEST_COMPRESSION, nullptr, 0, nullptr, 0) != FALSE;
        fclose(in);
        if (!added) {
            abortArchive();
            SetError(error, "Unable to add file to skin package");
            return false;
        }
    }
    if (ec || !mz_zip_writer_finalize_archive(&zip)) {
        abortArchive();
        SetError(error, "Unable to finalize skin package");
        return false;
    }
    mz_zip_writer_end(&zip);
    fclose(package);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace rivan::skin
