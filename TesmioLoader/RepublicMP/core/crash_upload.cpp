// SPDX-License-Identifier: GPL-3.0-only
// See crash_upload.h.
#include "crash_upload.h"
#include "diag_log.h"
#include "version.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../transport/lan_discovery.h"
#include "../transport/steam_p2p.h"
#include "../third_party/miniz/miniz.h"
#include "../third_party/miniz/miniz_zip.h"

#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

constexpr ULONGLONG kDesyncCooldownMs = 300000ULL; // 5 minutes between live DESYNC uploads
constexpr size_t kLogCount = 5;

struct LogSpec {
    const char* fileName;
    size_t prevCrashCap;
    size_t desyncCap;
    bool allowBaseFallback;
};

constexpr LogSpec kLogSpecs[kLogCount] = {
    { "RepublicMP_LastCrash.log",        384u * 1024u, 256u * 1024u, false },
    { "RepublicMP_Diagnostics.prev.log", 512u * 1024u, 256u * 1024u, false },
    { "RepublicMP_Diagnostics.log",      256u * 1024u, 512u * 1024u, false },
    { "tesmioloader.prev.log",           256u * 1024u, 128u * 1024u, true  },
    { "tesmioloader.log",                128u * 1024u, 256u * 1024u, true  }
};

volatile LONG g_uploadInFlight = 0;
volatile LONG g_prevCrashChecked = 0;
ULONGLONG g_lastDesyncUploadMs = 0;

void AppendJsonEscaped(std::string& dst, const char* s)
{
    dst.push_back('"');
    if (s) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
            unsigned char c = *p;
            if (c == '"' || c == '\\') {
                dst.push_back('\\');
                dst.push_back(static_cast<char>(c));
            } else if (c == '\n') {
                dst.append("\\n");
            } else if (c == '\r') {
                dst.append("\\r");
            } else if (c == '\t') {
                dst.append("\\t");
            } else if (c >= 0x20u) {
                dst.push_back(static_cast<char>(c));
            }
        }
    }
    dst.push_back('"');
}

std::string BuildReportInfoJson(const CrashUploadMeta& meta)
{
    char numBuf[128] = {};
    std::string js;
    js.reserve(512);
    js.append("{\n  \"reason\": ");
    AppendJsonEscaped(js, meta.reason[0] ? meta.reason : "OTHER");
    js.append(",\n  \"role\": ");
    AppendJsonEscaped(js, meta.role[0] ? meta.role : "UNKNOWN");
    js.append(",\n  \"version\": ");
    AppendJsonEscaped(js, meta.version[0] ? meta.version : REPUBLICMP_VERSION);
    _snprintf_s(numBuf, sizeof(numBuf), _TRUNCATE, "%llu", meta.tick);
    js.append(",\n  \"tick\": ").append(numBuf);
    _snprintf_s(numBuf, sizeof(numBuf), _TRUNCATE, "\"%08X\"", meta.dllCrc);
    js.append(",\n  \"dll_crc\": ").append(numBuf);
    _snprintf_s(numBuf, sizeof(numBuf), _TRUNCATE, "\"0x%08X\"", meta.peStamp);
    js.append(",\n  \"pe_stamp\": ").append(numBuf);
    _snprintf_s(numBuf, sizeof(numBuf), _TRUNCATE, "\"0x%04X\"", meta.dlcHost);
    js.append(",\n  \"dlc_host\": ").append(numBuf);
    _snprintf_s(numBuf, sizeof(numBuf), _TRUNCATE, "\"0x%04X\"", meta.dlcClient);
    js.append(",\n  \"dlc_client\": ").append(numBuf);
    _snprintf_s(numBuf, sizeof(numBuf), _TRUNCATE, "\"0x%04X\"", meta.dlcCommon);
    js.append(",\n  \"dlc_common\": ").append(numBuf);
    _snprintf_s(numBuf, sizeof(numBuf), _TRUNCATE, "%d", meta.twoStatesMode);
    js.append(",\n  \"two_states_mode\": ").append(numBuf);
    js.append(",\n  \"player_tag\": ");
    AppendJsonEscaped(js, meta.playerTag);
    js.append(",\n  \"desync_msg\": ");
    AppendJsonEscaped(js, meta.desyncMsg);
    js.append("\n}\n");
    return js;
}

bool TryReadSpecTail(const char* gameDir, const char* baseDir, const LogSpec& spec,
                     size_t maxBytes, std::vector<unsigned char>& outBuf)
{
    char path[MAX_PATH] = {};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", gameDir, spec.fileName);
    DiagLog_ReadFileTail(path, maxBytes, outBuf);
    if (outBuf.empty() && spec.allowBaseFallback && baseDir && baseDir[0]) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", baseDir, spec.fileName);
        DiagLog_ReadFileTail(path, maxBytes, outBuf);
    }
    return !outBuf.empty();
}

bool TryBuildZipWithScale(const char* gameDir, const char* baseDir,
                          const CrashUploadMeta& meta, size_t scaleDivisor,
                          std::vector<unsigned char>& outZip)
{
    outZip.clear();
    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        return false;
    }

    const std::string infoJson = BuildReportInfoJson(meta);
    bool ok = mz_zip_writer_add_mem(&zip, "report_info.json",
                                    infoJson.data(), infoJson.size(),
                                    MZ_DEFAULT_COMPRESSION) != 0;

    const bool isDesync = (_stricmp(meta.reason, "DESYNC") == 0);
    int logFilesAdded = 0;
    for (size_t i = 0; ok && i < kLogCount; ++i) {
        size_t cap = isDesync ? kLogSpecs[i].desyncCap : kLogSpecs[i].prevCrashCap;
        cap /= (scaleDivisor > 0 ? scaleDivisor : 1);
        if (cap < 16u * 1024u) cap = 16u * 1024u;

        std::vector<unsigned char> buf;
        if (!TryReadSpecTail(gameDir, baseDir, kLogSpecs[i], cap, buf)) {
            continue;
        }
        ok = mz_zip_writer_add_mem(&zip, kLogSpecs[i].fileName,
                                   buf.data(), buf.size(),
                                   MZ_DEFAULT_COMPRESSION) != 0;
        if (ok) ++logFilesAdded;
    }

    void* pBuf = nullptr;
    size_t zipSize = 0;
    if (ok && logFilesAdded > 0 && mz_zip_writer_finalize_heap_archive(&zip, &pBuf, &zipSize)) {
        outZip.assign(static_cast<const unsigned char*>(pBuf),
                      static_cast<const unsigned char*>(pBuf) + zipSize);
    } else {
        ok = false;
    }
    if (pBuf) {
        mz_free(pBuf);
    }
    mz_zip_writer_end(&zip);
    return ok && logFilesAdded > 0 && !outZip.empty();
}

void Fnv1aMixBytes(uint64_t& h, const void* ptr, size_t len)
{
    const unsigned char* p = static_cast<const unsigned char*>(ptr);
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
}

bool TryGetFileStat(const char* fullPath, WIN32_FILE_ATTRIBUTE_DATA& outAttr)
{
    if (!fullPath || !fullPath[0]) return false;
    return GetFileAttributesExA(fullPath, GetFileExInfoStandard, &outAttr) != FALSE &&
           (outAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string ResolveMarkerPath(const char* gameDir)
{
    if (!gameDir || !gameDir[0]) return "";
    return std::string(gameDir) + "\\republicmp_crash_sent.dat";
}

bool IsCrashUploadDisabledByEnv()
{
    char buf[16] = {};
    DWORD n = GetEnvironmentVariableA("REPUBLICMP_DISABLE_CRASH_UPLOAD", buf, sizeof(buf));
    return n > 0 && buf[0] == '1';
}

const char* ResolveActiveUploadUrl()
{
    const char* custom = TwoStates_GetCrashUploadUrl();
    if (custom && custom[0]) return custom;
    return REPUBLICMP_CRASH_UPLOAD_URL;
}

unsigned int ReadLocalPeTimestamp()
{
    HMODULE hExe = GetModuleHandleA(nullptr);
    if (!hExe) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hExe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(hExe) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

void FillPlayerTag(char* outTag, size_t outCap)
{
    std::string stmName = SteamP2P_GetLocalPersonaNameCopy();
    uint64_t stmId = SteamP2P_GetLocalSteamId();
    if (!stmName.empty() && stmId != 0) {
        _snprintf_s(outTag, outCap, _TRUNCATE, "%s (%llu)", stmName.c_str(), static_cast<unsigned long long>(stmId));
        return;
    }
    std::string localName = LanDiscovery_GetLocalDisplayName();
    if (!localName.empty()) {
        _snprintf_s(outTag, outCap, _TRUNCATE, "%s (LAN)", localName.c_str());
        return;
    }
    strncpy_s(outTag, outCap, "Player", _TRUNCATE);
}

std::wstring ToWideUtf8(const char* s)
{
    if (!s || !s[0]) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], needed);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

void BuildExtraHeaders(const char* ingestKey, const CrashUploadMeta& meta, std::wstring& outHdrs)
{
    std::string raw;
    raw.reserve(256);
    raw.append("Content-Type: application/zip\r\n");
    if (ingestKey && ingestKey[0]) {
        raw.append("X-RepublicMP-Key: ").append(ingestKey).append("\r\n");
    }
    raw.append("X-RepublicMP-Reason: ").append(meta.reason[0] ? meta.reason : "OTHER").append("\r\n");
    raw.append("X-RepublicMP-Version: ").append(meta.version[0] ? meta.version : REPUBLICMP_VERSION).append("\r\n");
    raw.append("X-RepublicMP-Role: ").append(meta.role[0] ? meta.role : "UNKNOWN").append("\r\n");
    outHdrs = ToWideUtf8(raw.c_str());
}

struct UploadWorkerJob {
    std::string gameDir;
    std::string baseDir;
    std::string uploadUrl;
    CrashUploadMeta meta;
    uint64_t prevFingerprint;
    uint64_t lastCrashOnlyFingerprint;
    bool hasPrevFingerprint;
};

DWORD WINAPI CrashUploadWorkerProc(LPVOID param)
{
    std::unique_ptr<UploadWorkerJob> job(static_cast<UploadWorkerJob*>(param));
    if (job && !job->hasPrevFingerprint) {
        Sleep(250);
    } else if (job) {
        Sleep(2000);
    }

    if (job && !job->gameDir.empty() && !job->uploadUrl.empty()) {
        std::vector<unsigned char> zipData;
        if (CrashUpload_BuildReportZip(job->gameDir.c_str(), job->baseDir.c_str(), job->meta, zipData)) {
            unsigned long status = 0;
            bool ok = CrashUpload_PostZipToUrl(job->uploadUrl.c_str(), REPUBLICMP_CRASH_INGEST_KEY,
                                               job->meta, zipData.data(), zipData.size(), &status);
            if (ok && job->hasPrevFingerprint) {
                if (job->prevFingerprint != 0) {
                    CrashUpload_MarkFingerprintUploaded(job->gameDir.c_str(), job->prevFingerprint);
                }
                if (job->lastCrashOnlyFingerprint != 0 &&
                    job->lastCrashOnlyFingerprint != job->prevFingerprint) {
                    CrashUpload_MarkFingerprintUploaded(job->gameDir.c_str(), job->lastCrashOnlyFingerprint);
                }
            }
            DiagLog_Printf("CRASH_UP", "Upload %s (reason=%s, role=%s, zip=%u B, http=%lu)",
                           ok ? "succeeded" : "skipped/failed",
                           job->meta.reason, job->meta.role,
                           static_cast<unsigned int>(zipData.size()), status);
        }
    }

    InterlockedExchange(&g_uploadInFlight, 0);
    return 0;
}

bool SpawnWorkerJob(std::unique_ptr<UploadWorkerJob> job)
{
    if (InterlockedCompareExchange(&g_uploadInFlight, 1, 0) != 0) {
        return false;
    }
    HANDLE hThread = CreateThread(nullptr, 0, CrashUploadWorkerProc, job.get(), 0, nullptr);
    if (!hThread) {
        InterlockedExchange(&g_uploadInFlight, 0);
        return false;
    }
    job.release();
    CloseHandle(hThread);
    return true;
}

} // namespace

bool CrashUpload_BuildReportZip(const char* gameDir, const char* baseDir,
                                const CrashUploadMeta& meta, std::vector<unsigned char>& outZip)
{
    outZip.clear();
    if (!gameDir || !gameDir[0]) return false;

    for (size_t scale = 1; scale <= 8; scale *= 2) {
        std::vector<unsigned char> candidate;
        if (!TryBuildZipWithScale(gameDir, baseDir, meta, scale, candidate)) {
            return false;
        }
        if (candidate.size() <= CRASH_UPLOAD_MAX_ZIP_BYTES) {
            outZip.swap(candidate);
            return true;
        }
    }
    return false;
}

uint64_t CrashUpload_ComputeFingerprintForFlags(const char* gameDir, const char* baseDir, unsigned int flags)
{
    if (!gameDir || !gameDir[0] || flags == 0) return 0;

    struct FingerprintSpec {
        unsigned int flagBit;
        const char*  fileName;
        const char*  fallbackFileName;
        bool         useLoaderDir;
    };
    static const FingerprintSpec kSpecs[3] = {
        { DIAG_PREV_CRASH_LAST_FILE,   "RepublicMP_LastCrash.log",        nullptr,            false },
        { DIAG_PREV_CRASH_DIAG_PREV,   "RepublicMP_Diagnostics.prev.log", nullptr,            false },
        { DIAG_PREV_CRASH_LOADER_PREV, "tesmioloader.prev.log",           "tesmioloader.log", true  }
    };

    uint64_t h = 14695981039346656037ULL;
    bool anyFound = false;

    for (const FingerprintSpec& spec : kSpecs) {
        if ((flags & spec.flagBit) == 0) continue;
        const char* primaryDir = (spec.useLoaderDir && baseDir && baseDir[0]) ? baseDir : gameDir;
        const char* names[2] = { spec.fileName, spec.fallbackFileName };
        for (int n = 0; n < 2; ++n) {
            if (!names[n]) continue;
            char full[MAX_PATH] = {};
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", primaryDir, names[n]);
            bool found = TryGetFileStat(full, fad);
            if (!found && spec.useLoaderDir && primaryDir != gameDir) {
                _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", gameDir, names[n]);
                found = TryGetFileStat(full, fad);
            }
            if (!found || (fad.nFileSizeHigh == 0 && fad.nFileSizeLow == 0)) continue;

            anyFound = true;
            Fnv1aMixBytes(h, spec.fileName, strlen(spec.fileName));
            Fnv1aMixBytes(h, &fad.nFileSizeHigh, sizeof(fad.nFileSizeHigh));
            Fnv1aMixBytes(h, &fad.nFileSizeLow, sizeof(fad.nFileSizeLow));
            Fnv1aMixBytes(h, &fad.ftLastWriteTime, sizeof(fad.ftLastWriteTime));
            break;
        }
    }

    if (!anyFound) return 0;
    return h != 0 ? h : 1ULL;
}

uint64_t CrashUpload_ComputePrevCrashFingerprint(const char* gameDir, const char* baseDir)
{
    const unsigned int flags = DiagLog_DetectPrevCrashFlagsForDir(gameDir, baseDir);
    if (flags == 0) return 0;
    return CrashUpload_ComputeFingerprintForFlags(gameDir, baseDir, flags);
}

bool CrashUpload_HasUploadedFingerprint(const char* gameDir, uint64_t fp)
{
    if (!gameDir || !gameDir[0] || fp == 0) return false;
    std::string marker = ResolveMarkerPath(gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, marker.c_str(), "r") != 0 || !f) return false;
    char line[64] = {};
    bool matched = false;
    for (int i = 0; i < 8 && fgets(line, sizeof(line), f); ++i) {
        const uint64_t saved = static_cast<uint64_t>(_strtoui64(line, nullptr, 16));
        if (saved != 0 && saved == fp) {
            matched = true;
            break;
        }
    }
    fclose(f);
    return matched;
}

void CrashUpload_MarkFingerprintUploaded(const char* gameDir, uint64_t fp)
{
    if (!gameDir || !gameDir[0] || fp == 0) return;
    std::string marker = ResolveMarkerPath(gameDir);
    std::vector<uint64_t> history;
    FILE* rf = nullptr;
    if (fopen_s(&rf, marker.c_str(), "r") == 0 && rf) {
        char line[64] = {};
        for (int i = 0; i < 8 && fgets(line, sizeof(line), rf); ++i) {
            const uint64_t saved = static_cast<uint64_t>(_strtoui64(line, nullptr, 16));
            if (saved != 0 && saved != fp) {
                history.push_back(saved);
            }
        }
        fclose(rf);
    }
    while (history.size() > 3) {
        history.erase(history.begin());
    }
    history.push_back(fp);

    FILE* f = nullptr;
    if (fopen_s(&f, marker.c_str(), "w") != 0 || !f) return;
    for (uint64_t v : history) {
        fprintf(f, "%016llX\n", static_cast<unsigned long long>(v));
    }
    fclose(f);
}

bool CrashUpload_PostZipToUrl(const char* urlUtf8, const char* ingestKey,
                              const CrashUploadMeta& meta, const unsigned char* zipData,
                              size_t zipSize, unsigned long* outHttpStatus)
{
    if (outHttpStatus) *outHttpStatus = 0;
    if (!urlUtf8 || !urlUtf8[0] || !zipData || zipSize == 0 || zipSize > CRASH_UPLOAD_MAX_ZIP_BYTES) {
        return false;
    }

    std::wstring wUrl = ToWideUtf8(urlUtf8);
    if (wUrl.empty()) return false;

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[512] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 512;
    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &uc) || host[0] == L'\0') {
        return false;
    }
    if (path[0] == L'\0') {
        wcscpy_s(path, L"/");
    }

    HINTERNET hSession = WinHttpOpen(L"RepublicMP-CrashReport/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 3000, 4000, 8000, 8000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring hdrs;
    BuildExtraHeaders(ingestKey, meta, hdrs);

    bool ok = false;
    void* mutablePayload = const_cast<unsigned char*>(zipData);
    const DWORD bodyBytes = static_cast<DWORD>(zipSize);
    if (WinHttpSendRequest(hRequest, hdrs.c_str(), static_cast<DWORD>(hdrs.size()),
                           mutablePayload, bodyBytes, bodyBytes, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                WINHTTP_NO_HEADER_INDEX)) {
            if (outHttpStatus) *outHttpStatus = statusCode;
            ok = (statusCode >= 200 && statusCode < 300);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

void CrashUpload_InitAndCheckPrevCrashAsync(void)
{
    if (InterlockedCompareExchange(&g_prevCrashChecked, 1, 0) != 0) return;
    if (!TwoStates_GetUploadCrashReports() || IsCrashUploadDisabledByEnv()) return;

    const char* gameDir = DiagLog_GetGameDir();
    const char* baseDir = DiagLog_GetLoaderBaseDir();
    if (!gameDir || !gameDir[0]) return;
    if (!DiagLog_DetectPrevCrashOrDesync()) return;

    const uint64_t fp = CrashUpload_ComputePrevCrashFingerprint(gameDir, baseDir);
    if (fp != 0 && CrashUpload_HasUploadedFingerprint(gameDir, fp)) {
        return;
    }

    auto job = std::make_unique<UploadWorkerJob>();
    job->gameDir = gameDir;
    job->baseDir = baseDir ? baseDir : "";
    job->uploadUrl = ResolveActiveUploadUrl();
    job->prevFingerprint = fp;
    job->lastCrashOnlyFingerprint =
        CrashUpload_ComputeFingerprintForFlags(gameDir, baseDir, DIAG_PREV_CRASH_LAST_FILE);
    job->hasPrevFingerprint = true;

    strncpy_s(job->meta.reason, "PREV_CRASH", _TRUNCATE);
    strncpy_s(job->meta.role, SaveSync_IsHostRole() ? "HOST" : "MENU", _TRUNCATE);
    strncpy_s(job->meta.version, REPUBLICMP_VERSION, _TRUNCATE);
    job->meta.tick = 0;
    job->meta.dllCrc = SaveSync_GetLocalDllCrc();
    job->meta.peStamp = ReadLocalPeTimestamp();
    SaveSync_GetDlcMasks(&job->meta.dlcHost, &job->meta.dlcClient, &job->meta.dlcCommon);
    job->meta.twoStatesMode = static_cast<int>(TwoStates_GetMode());
    FillPlayerTag(job->meta.playerTag, sizeof(job->meta.playerTag));

    SpawnWorkerJob(std::move(job));
}

void CrashUpload_NotifyDesyncAsync(const char* desyncMsg, unsigned long long tick)
{
    if (!TwoStates_GetUploadCrashReports() || IsCrashUploadDisabledByEnv()) return;

    const char* gameDir = DiagLog_GetGameDir();
    if (!gameDir || !gameDir[0]) return;

    const ULONGLONG now = GetTickCount64();
    if (g_lastDesyncUploadMs != 0 && (now - g_lastDesyncUploadMs) < kDesyncCooldownMs) {
        return;
    }
    g_lastDesyncUploadMs = now;

    auto job = std::make_unique<UploadWorkerJob>();
    job->gameDir = gameDir;
    const char* baseDir = DiagLog_GetLoaderBaseDir();
    job->baseDir = baseDir ? baseDir : "";
    job->uploadUrl = ResolveActiveUploadUrl();
    job->prevFingerprint = 0;
    job->hasPrevFingerprint = false;

    strncpy_s(job->meta.reason, "DESYNC", _TRUNCATE);
    strncpy_s(job->meta.role, SaveSync_IsHostRole() ? "HOST" : "CLIENT", _TRUNCATE);
    strncpy_s(job->meta.version, REPUBLICMP_VERSION, _TRUNCATE);
    if (desyncMsg && desyncMsg[0]) {
        strncpy_s(job->meta.desyncMsg, desyncMsg, _TRUNCATE);
    }
    job->meta.tick = tick;
    job->meta.dllCrc = SaveSync_GetLocalDllCrc();
    job->meta.peStamp = ReadLocalPeTimestamp();
    SaveSync_GetDlcMasks(&job->meta.dlcHost, &job->meta.dlcClient, &job->meta.dlcCommon);
    job->meta.twoStatesMode = static_cast<int>(TwoStates_GetMode());
    FillPlayerTag(job->meta.playerTag, sizeof(job->meta.playerTag));

    SpawnWorkerJob(std::move(job));
}
