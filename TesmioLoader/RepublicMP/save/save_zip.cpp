// SPDX-License-Identifier: GPL-3.0-only
// See save_zip.h.
#include "save_zip.h"
#include "../third_party/miniz/miniz.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

void SaveZip_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static std::string PathToUtf8(const fs::path& p)
{
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

bool ZipSaveDirectory(const char* srcDir, const char* outZipPath, ZipProgressFn onProgress, void* userData)
{
    std::error_code ec;
    fs::path base(srcDir);
    if (!fs::exists(base, ec) || !fs::is_directory(base, ec)) {
        Logf("republicmp  SAVE_ZIP: folder not found or not a directory: [%s]", srcDir);
        return false;
    }

    size_t totalFiles = 0;
    if (onProgress) {
        std::error_code countEc;
        for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, countEc);
             it != fs::recursive_directory_iterator(); it.increment(countEc)) {
            if (countEc) break;
            std::error_code fileEc;
            if (it->is_regular_file(fileEc) && !fileEc) totalFiles++;
        }
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    fs::path outPathFs(outZipPath);
    std::string outZipPathUtf8 = PathToUtf8(outPathFs);
    if (!mz_zip_writer_init_file_v2(&zip, outZipPathUtf8.c_str(), 0, MZ_ZIP_FLAG_WRITE_ZIP64)) {
        Logf("republicmp  SAVE_ZIP: mz_zip_writer_init_file_v2 couldn't open [%s]", outZipPath);
        return false;
    }

    size_t fileCount = 0;
    long long totalBytes = 0;
    for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            Logf("republicmp  SAVE_ZIP: folder walk error: %s - aborting archive creation", ec.message().c_str());
            mz_zip_writer_end(&zip);
            std::error_code rmEc;
            fs::remove(outPathFs, rmEc);
            return false;
        }
        const fs::directory_entry& entry = *it;
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) {
            continue;
        }

        fs::path relPath = fs::relative(entry.path(), base, ec);
        if (ec) {
            Logf("republicmp  SAVE_ZIP: couldn't compute relative path for [%s]", entry.path().string().c_str());
            mz_zip_writer_end(&zip);
            std::error_code rmEc;
            fs::remove(outPathFs, rmEc);
            return false;
        }

        auto u8Archive = relPath.generic_u8string();
        std::string archiveNameUtf8(reinterpret_cast<const char*>(u8Archive.data()), u8Archive.size());
        std::string fullPathUtf8 = PathToUtf8(entry.path());

        if (!mz_zip_writer_add_file(&zip, archiveNameUtf8.c_str(), fullPathUtf8.c_str(), nullptr, 0, MZ_DEFAULT_LEVEL)) {
            Logf("republicmp  SAVE_ZIP: couldn't add [%s]", archiveNameUtf8.c_str());
            mz_zip_writer_end(&zip);
            std::error_code rmEc;
            fs::remove(outPathFs, rmEc);
            return false;
        }
        fileCount++;
        totalBytes += (long long)entry.file_size(fileEc);
        if (onProgress) onProgress(fileCount, totalFiles, userData);
    }

    if (!mz_zip_writer_finalize_archive(&zip)) {
        Logf("republicmp  SAVE_ZIP: finalize archive failed for [%s]", outZipPath);
        mz_zip_writer_end(&zip);
        std::error_code rmEc;
        fs::remove(outPathFs, rmEc);
        return false;
    }
    mz_zip_writer_end(&zip);

    long long zipSize = 0;
    std::error_code sizeEc;
    zipSize = (long long)fs::file_size(outPathFs, sizeEc);

    Logf("republicmp  SAVE_ZIP: zipped %zu files (%lld bytes) from [%s] -> [%s] (%lld bytes archive)",
         fileCount, totalBytes, srcDir, outZipPath, zipSize);
    return true;
}

// The archive comes from the other player: an entry that is absolute
// ("C:\...", "\...") or climbs out with ".." would be written outside destDir.
static bool IsSafeEntryName(const char* name)
{
    if (!name || !*name) return false;
    fs::path p(name);
    if (p.has_root_name() || p.has_root_directory()) return false;
    for (const fs::path& part : p) {
        if (part == "..") return false;
    }
    return true;
}

bool UnzipToDirectory(const char* zipPath, const char* destDir, ZipProgressFn onProgress, void* userData)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    fs::path zipPathFs(zipPath);
    std::string zipPathUtf8 = PathToUtf8(zipPathFs);
    if (!mz_zip_reader_init_file(&zip, zipPathUtf8.c_str(), 0)) {
        Logf("republicmp  SAVE_ZIP: mz_zip_reader_init_file couldn't open [%s]", zipPath);
        return false;
    }

    std::error_code ec;
    fs::path destBase(destDir);
    fs::create_directories(destBase, ec);

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    size_t extracted = 0;
    for (mz_uint i = 0; i < numFiles; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            Logf("republicmp  SAVE_ZIP: mz_zip_reader_file_stat failed for index %u", i);
            continue;
        }

        // A real save never contains such a name - refuse the whole archive.
        if (!IsSafeEntryName(stat.m_filename)) {
            Logf("republicmp  SAVE_ZIP: refusing unsafe entry [%s] in [%s]", stat.m_filename, zipPath);
            mz_zip_reader_end(&zip);
            return false;
        }

        fs::path relPath(reinterpret_cast<const char8_t*>(stat.m_filename));
        fs::path outPath = destBase / relPath;
        std::error_code dirEc;
        fs::create_directories(outPath.parent_path(), dirEc);

        std::string outPathUtf8 = PathToUtf8(outPath);
        if (!mz_zip_reader_extract_to_file(&zip, i, outPathUtf8.c_str(), 0)) {
            Logf("republicmp  SAVE_ZIP: couldn't extract [%s] -> [%s]", stat.m_filename, outPathUtf8.c_str());
            mz_zip_reader_end(&zip);
            return false;
        }
        extracted++;
        if (onProgress) onProgress(i + 1, numFiles, userData);
    }

    mz_zip_reader_end(&zip);
    Logf("republicmp  SAVE_ZIP: extracted %zu files from [%s] -> [%s]", extracted, zipPath, destDir);
    return true;
}
