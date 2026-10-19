// SPDX-License-Identifier: GPL-3.0-only
// Zip/unzip an entire save folder, using vendored miniz.
#pragma once

#include "tesmio_plugin.h"

void SaveZip_Bind(const TsmHost* host);

typedef void (*ZipProgressFn)(size_t done, size_t total, void* userData);

bool ZipSaveDirectory(const char* srcDir, const char* outZipPath,
                      ZipProgressFn onProgress = nullptr, void* userData = nullptr);
bool UnzipToDirectory(const char* zipPath, const char* destDir,
                     ZipProgressFn onProgress = nullptr, void* userData = nullptr);
