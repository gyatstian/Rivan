// Rivan source file
// Purpose: Safe ZIP packaging used by .rivanskin files.
#include "SkinArchive.h"

#include "miniz/miniz.h"

#include <Windows.h>

#include <cstdint>
#include <system_error>

namespace rivan::skin {
namespace {

constexpr std::uint64_t kMaximumPackageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumPackageFiles = 512;

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

bool SafeArchivePath(const char* name, std::filesystem::path* result) {
    if (name == nullptr || *name == '\0') return false;
    std::filesystem::path path(name);
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part.empty() || part == L"." || part == L"..") return false;
    }
    *result = std::move(path);
    return true;
}

} // namespace

bool ExtractSkinArchive(const std::filesystem::path& archive,
                        const std::filesystem::path& destination,
                        std::string* error) {
    mz_zip_archive zip{};
    const auto archiveName = Utf8(archive);
    if (!mz_zip_reader_init_file(&zip, archiveName.c_str(), 0)) {
        SetError(error, "Unable to open skin package");
        return false;
    }
    const auto close = [&] { mz_zip_reader_end(&zip); };
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > kMaximumPackageFiles) {
        close();
        SetError(error, "Skin package exceeds the 512-file limit");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        close();
        SetError(error, "Unable to create skin cache directory: " + ec.message());
        return false;
    }
    std::uint64_t total = 0;
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat entry{};
        std::filesystem::path relative;
        if (!mz_zip_reader_file_stat(&zip, index, &entry) || !SafeArchivePath(entry.m_filename, &relative)) {
            close();
            SetError(error, "Skin package contains an unsafe path");
            return false;
        }
        const auto target = destination / relative;
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
        if (ec || !mz_zip_reader_extract_to_file(&zip, index, Utf8(target).c_str(), 0)) {
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
    mz_zip_archive zip{};
    const auto archiveName = Utf8(archive);
    if (!mz_zip_writer_init_file(&zip, archiveName.c_str(), 0)) {
        SetError(error, "Unable to create skin package");
        return false;
    }
    std::error_code ec;
    std::uint64_t total = 0;
    std::size_t count = 0;
    for (std::filesystem::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        const auto size = it->file_size(ec);
        if (ec || ++count > kMaximumPackageFiles || size > kMaximumPackageBytes - total) {
            mz_zip_writer_end(&zip); std::filesystem::remove(archive, ec);
            SetError(error, "Skin package exceeds the 512-file or 64 MiB limit"); return false;
        }
        total += size;
        const auto relative = std::filesystem::relative(it->path(), source, ec);
        const auto entryName = Utf8(relative.generic_wstring());
        const auto sourceName = Utf8(it->path());
        if (ec || !mz_zip_writer_add_file(&zip, entryName.c_str(), sourceName.c_str(), nullptr, 0,
                                          MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&zip); std::filesystem::remove(archive, ec);
            SetError(error, "Unable to add file to skin package"); return false;
        }
    }
    if (ec || !mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip); std::filesystem::remove(archive, ec);
        SetError(error, "Unable to finalize skin package"); return false;
    }
    mz_zip_writer_end(&zip);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace rivan::skin
