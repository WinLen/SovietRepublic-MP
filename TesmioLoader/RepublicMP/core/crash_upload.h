// SPDX-License-Identifier: GPL-3.0-only
// Background crash and desync report packager & uploader for RepublicMP.
// Packages diagnostic/loader logs into a bounded ZIP archive and submits them
// asynchronously over WinHTTP to the dedicated collector service on 5.44.40.46.
// Never contains Google Drive tokens/credentials and never runs WinHTTP from VEH.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

#ifndef REPUBLICMP_CRASH_UPLOAD_URL
#define REPUBLICMP_CRASH_UPLOAD_URL "http://5.44.40.46/republicmp-logs/v1/upload"
#endif

// Write-only collector spam-filter header value (grants zero read or Google Drive access).
#ifndef REPUBLICMP_CRASH_INGEST_KEY
#define REPUBLICMP_CRASH_INGEST_KEY "7e4c9a12b8f03d61e52a94c80f73d1b6"
#endif

#ifndef CRASH_UPLOAD_MAX_ZIP_BYTES
#define CRASH_UPLOAD_MAX_ZIP_BYTES (1800u * 1024u)
#endif

struct CrashUploadMeta {
    char reason[24];
    char role[16];
    char version[32];
    char desyncMsg[256];
    unsigned long long tick;
    unsigned int dllCrc;
    unsigned int peStamp;
    uint32_t dlcHost;
    uint32_t dlcClient;
    uint32_t dlcCommon;
    int twoStatesMode;
    char playerTag[96];
};

// Pure / testable helpers
bool CrashUpload_BuildReportZip(const char* gameDir, const char* baseDir,
                                const CrashUploadMeta& meta, std::vector<unsigned char>& outZip);
uint64_t CrashUpload_ComputePrevCrashFingerprint(const char* gameDir, const char* baseDir);
bool CrashUpload_HasUploadedFingerprint(const char* gameDir, uint64_t fp);
void CrashUpload_MarkFingerprintUploaded(const char* gameDir, uint64_t fp);
bool CrashUpload_PostZipToUrl(const char* urlUtf8, const char* ingestKey,
                              const CrashUploadMeta& meta, const unsigned char* zipData,
                              size_t zipSize, unsigned long* outHttpStatus);

// Runtime hooks (called from TsmPluginStart and LockstepNet on main thread)
void CrashUpload_InitAndCheckPrevCrashAsync(void);
void CrashUpload_NotifyDesyncAsync(const char* desyncMsg, unsigned long long tick);
