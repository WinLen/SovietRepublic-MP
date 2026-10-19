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
    if (!mz_zip_writer_init_file(&zip, outZipPath, 0)) {
        Logf("republicmp  SAVE_ZIP: mz_zip_writer_init_file couldn't open [%s]", outZipPath);
        return false;
    }

    size_t fileCount = 0;
    long long totalBytes = 0;
    for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            Logf("republicmp  SAVE_ZIP: folder walk error: %s", ec.message().c_str());
            break;
        }
        const fs::directory_entry& entry = *it;
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) {
            continue;
        }

        std::string archiveName = fs::relative(entry.path(), base, ec).generic_string();
        std::string fullPath = entry.path().string();
        if (ec) {
            Logf("republicmp  SAVE_ZIP: couldn't compute relative path for [%s]", fullPath.c_str());
            continue;
        }

        if (!mz_zip_writer_add_file(&zip, archiveName.c_str(), fullPath.c_str(), nullptr, 0, MZ_DEFAULT_LEVEL)) {
            Logf("republicmp  SAVE_ZIP: couldn't add [%s]", archiveName.c_str());
            mz_zip_writer_end(&zip);
            return false;
        }
        fileCount++;
        totalBytes += (long long)entry.file_size(fileEc);
        if (onProgress) onProgress(fileCount, totalFiles, userData);
    }

    if (!mz_zip_writer_finalize_archive(&zip)) {
        Logf("republicmp  SAVE_ZIP: finalize archive failed for [%s]", outZipPath);
        mz_zip_writer_end(&zip);
        return false;
    }
    mz_zip_writer_end(&zip);

    long long zipSize = 0;
    std::error_code sizeEc;
    zipSize = (long long)fs::file_size(fs::path(outZipPath), sizeEc);

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
    if (!mz_zip_reader_init_file(&zip, zipPath, 0)) {
        Logf("republicmp  SAVE_ZIP: mz_zip_reader_init_file couldn't open [%s]", zipPath);
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs::path(destDir), ec);

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

        fs::path outPath = fs::path(destDir) / stat.m_filename;
        std::error_code dirEc;
        fs::create_directories(outPath.parent_path(), dirEc);

        std::string outPathStr = outPath.string();
        if (!mz_zip_reader_extract_to_file(&zip, i, outPathStr.c_str(), 0)) {
            Logf("republicmp  SAVE_ZIP: couldn't extract [%s] -> [%s]", stat.m_filename, outPathStr.c_str());
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
