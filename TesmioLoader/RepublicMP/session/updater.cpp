// SPDX-License-Identifier: GPL-3.0-only
// See updater.h.
#include "updater.h"
#include "../core/version.h"
#include "../ui/ui_log.h"
#include "../third_party/miniz/miniz.h"

#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")

static CRITICAL_SECTION g_updLock;
static bool g_updLockInit = false;
static volatile UpdateStatus g_status = UPDATE_IDLE;
static char g_latestVersion[64] = "";
static char g_statusMessage[256] = "";
static char g_githubRepo[128] = REPUBLICMP_GITHUB_REPO;
static char g_downloadUrl[512] = "";
static char g_workshopDllPath[MAX_PATH] = "";
static volatile bool g_workerRunning = false;

static void EnsureLock()
{
    if (!g_updLockInit) {
        InitializeCriticalSection(&g_updLock);
        g_updLockInit = true;
    }
}

static std::string GetOwnDllPath()
{
    char path[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&GetOwnDllPath, &hMod)) {
        GetModuleFileNameA(hMod, path, MAX_PATH);
    }
    return std::string(path);
}

static std::string GetUpdaterIniPath()
{
    std::string dll = GetOwnDllPath();
    size_t pos = dll.find_last_of("\\/");
    if (pos == std::string::npos) return "republicmp_updater.ini";
    return dll.substr(0, pos + 1) + "republicmp_updater.ini";
}

static void LoadConfig()
{
    std::string ini = GetUpdaterIniPath();
    char buf[128] = {};
    GetPrivateProfileStringA("updater", "github_repo", REPUBLICMP_GITHUB_REPO, buf, sizeof(buf), ini.c_str());
    EnterCriticalSection(&g_updLock);
    if (buf[0]) strncpy_s(g_githubRepo, buf, _TRUNCATE);
    LeaveCriticalSection(&g_updLock);
}

static void SaveConfig()
{
    std::string ini = GetUpdaterIniPath();
    EnterCriticalSection(&g_updLock);
    WritePrivateProfileStringA("updater", "github_repo", g_githubRepo, ini.c_str());
    WritePrivateProfileStringA("updater", "current_version", REPUBLICMP_VERSION, ini.c_str());
    LeaveCriticalSection(&g_updLock);
}

static void SetStatus(UpdateStatus st, const char* msg)
{
    EnterCriticalSection(&g_updLock);
    g_status = st;
    if (msg) strncpy_s(g_statusMessage, msg, _TRUNCATE);
    LeaveCriticalSection(&g_updLock);
}

static unsigned int ComputeFileCrc(const char* path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return 0;
    mz_uint32 crc = MZ_CRC32_INIT;
    unsigned char buf[16384];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = mz_crc32(crc, buf, n);
    }
    fclose(f);
    return (unsigned int)crc;
}

// Extracts a JSON string value for a given key (minimal parser for GitHub API response).
static std::string ExtractJsonString(const std::string& json, const char* key, size_t startPos = 0)
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t kpos = json.find(pattern, startPos);
    if (kpos == std::string::npos) return "";
    size_t colon = json.find(':', kpos + pattern.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    std::string out;
    for (size_t i = q1 + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            out.push_back(json[++i]);
        } else if (json[i] == '"') {
            break;
        } else {
            out.push_back(json[i]);
        }
    }
    return out;
}

static bool HttpGetBytes(const char* urlUtf8, std::vector<unsigned char>& outData)
{
    outData.clear();
    wchar_t wurl[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, urlUtf8, -1, wurl, 1024);

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[768] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 768;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"RepublicMP-Updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 4000, 4000, 8000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Follow redirects (needed for github.com release asset downloads -> objects.githubusercontent.com)
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    bool ok = false;
    if (WinHttpSendRequest(hRequest, L"Accept: application/vnd.github+json\r\n", -1L,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        if (statusCode == 200) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                size_t oldSz = outData.size();
                outData.resize(oldSz + avail);
                DWORD readBytes = 0;
                if (!WinHttpReadData(hRequest, outData.data() + oldSz, avail, &readBytes)) break;
                outData.resize(oldSz + readBytes);
            }
            ok = !outData.empty();
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

static std::string NormalizeVersion(const std::string& v)
{
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) return v.substr(1);
    return v;
}

// Checks local Steam Workshop folder (steamapps/workshop/content/784150/*/TesmioLoader/build/plugins/RepublicMP.dll)
static bool CheckWorkshopNewerDll(std::string& outDllPath)
{
    std::string ownDll = GetOwnDllPath();
    if (ownDll.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA ownAttr = {};
    if (!GetFileAttributesExA(ownDll.c_str(), GetFileExInfoStandard, &ownAttr)) return false;
    unsigned int ownCrc = ComputeFileCrc(ownDll.c_str());

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    // Walk up from common/SovietRepublic/SOVIET64.exe -> steamapps/workshop/content/784150
    std::error_code ec;
    auto steamapps = p.parent_path().parent_path().parent_path();
    auto workshop784150 = steamapps / "workshop" / "content" / "784150";
    if (!std::filesystem::exists(workshop784150, ec)) return false;

    for (auto const& entry : std::filesystem::directory_iterator(workshop784150, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto candidate = entry.path() / "TesmioLoader" / "build" / "plugins" / "RepublicMP.dll";
        if (!std::filesystem::exists(candidate, ec)) continue;
        std::string candStr = candidate.string();
        if (_stricmp(candStr.c_str(), ownDll.c_str()) == 0) continue;

        WIN32_FILE_ATTRIBUTE_DATA candAttr = {};
        if (!GetFileAttributesExA(candStr.c_str(), GetFileExInfoStandard, &candAttr)) continue;
        unsigned int candCrc = ComputeFileCrc(candStr.c_str());
        if (candCrc != 0 && candCrc != ownCrc &&
            CompareFileTime(&candAttr.ftLastWriteTime, &ownAttr.ftLastWriteTime) > 0) {
            outDllPath = candStr;
            return true;
        }
    }
    return false;
}

static DWORD WINAPI CheckThreadProc(LPVOID)
{
    SetStatus(UPDATE_CHECKING, "Проверка обновлений...");

    // 1. Check Steam Workshop folder first
    std::string wsDll;
    if (CheckWorkshopNewerDll(wsDll)) {
        EnterCriticalSection(&g_updLock);
        strncpy_s(g_workshopDllPath, wsDll.c_str(), _TRUNCATE);
        strncpy_s(g_latestVersion, "Steam Workshop", _TRUNCATE);
        LeaveCriticalSection(&g_updLock);
        SetStatus(UPDATE_AVAILABLE_WORKSHOP, "Найдена новая версия в папке Steam Workshop!");
        UiLog_Add("[Updater] Найдена обновлённая сборка в Steam Workshop");
        g_workerRunning = false;
        return 0;
    }

    // 2. Check GitHub Releases if repo is configured
    char repo[128] = {};
    EnterCriticalSection(&g_updLock);
    strncpy_s(repo, g_githubRepo, _TRUNCATE);
    LeaveCriticalSection(&g_updLock);

    if (!repo[0]) {
        SetStatus(UPDATE_IDLE, "Репозиторий GitHub не задан (укажите owner/repo для автообновления)");
        g_workerRunning = false;
        return 0;
    }

    char apiUrl[256];
    _snprintf_s(apiUrl, sizeof(apiUrl), _TRUNCATE, "https://api.github.com/repos/%s/releases/latest", repo);

    std::vector<unsigned char> body;
    if (!HttpGetBytes(apiUrl, body)) {
        SetStatus(UPDATE_ERROR, "Не удалось получить релизы с GitHub (проверьте owner/repo или интернет)");
        g_workerRunning = false;
        return 0;
    }

    std::string json((const char*)body.data(), body.size());
    std::string tag = ExtractJsonString(json, "tag_name");
    if (tag.empty()) {
        SetStatus(UPDATE_ERROR, "Ответ GitHub API не содержит tag_name");
        g_workerRunning = false;
        return 0;
    }

    // Find browser_download_url for RepublicMP.dll (preferred) or RepublicMP_Friend_Pack.zip
    std::string dllUrl;
    size_t searchPos = 0;
    while (true) {
        std::string url = ExtractJsonString(json, "browser_download_url", searchPos);
        if (url.empty()) break;
        if (url.find("RepublicMP.dll") != std::string::npos) {
            dllUrl = url;
            break;
        }
        size_t nextPos = json.find(url, searchPos);
        if (nextPos == std::string::npos) break;
        searchPos = nextPos + url.size();
    }

    std::string normRemote = NormalizeVersion(tag);
    std::string normLocal = NormalizeVersion(REPUBLICMP_VERSION);

    EnterCriticalSection(&g_updLock);
    strncpy_s(g_latestVersion, tag.c_str(), _TRUNCATE);
    strncpy_s(g_downloadUrl, dllUrl.c_str(), _TRUNCATE);
    LeaveCriticalSection(&g_updLock);

    if (normRemote != normLocal && !dllUrl.empty()) {
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "Доступно обновление на GitHub: %s (текущая v%s)", tag.c_str(), REPUBLICMP_VERSION);
        SetStatus(UPDATE_AVAILABLE_GITHUB, msg);
        UiLog_Add(msg);
    } else {
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "Установлена актуальная версия (v%s)", REPUBLICMP_VERSION);
        SetStatus(UPDATE_UP_TO_DATE, msg);
    }

    g_workerRunning = false;
    return 0;
}

static DWORD WINAPI ApplyThreadProc(LPVOID)
{
    UpdateStatus curStatus = Updater_GetStatus();
    std::string ownDll = GetOwnDllPath();
    if (ownDll.empty()) {
        SetStatus(UPDATE_ERROR, "Не удалось определить путь к RepublicMP.dll");
        g_workerRunning = false;
        return 0;
    }

    std::vector<unsigned char> newBytes;

    if (curStatus == UPDATE_AVAILABLE_WORKSHOP) {
        SetStatus(UPDATE_DOWNLOADING, "Копирование обновления из Steam Workshop...");
        char wsPath[MAX_PATH] = {};
        EnterCriticalSection(&g_updLock);
        strncpy_s(wsPath, g_workshopDllPath, _TRUNCATE);
        LeaveCriticalSection(&g_updLock);

        FILE* f = nullptr;
        if (fopen_s(&f, wsPath, "rb") == 0 && f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                newBytes.resize((size_t)sz);
                fread(newBytes.data(), 1, (size_t)sz, f);
            }
            fclose(f);
        }
    } else {
        SetStatus(UPDATE_DOWNLOADING, "Скачивание RepublicMP.dll с GitHub Releases...");
        char url[512] = {};
        EnterCriticalSection(&g_updLock);
        strncpy_s(url, g_downloadUrl, _TRUNCATE);
        LeaveCriticalSection(&g_updLock);

        if (!url[0] || !HttpGetBytes(url, newBytes)) {
            SetStatus(UPDATE_ERROR, "Ошибка при скачивании RepublicMP.dll с GitHub");
            g_workerRunning = false;
            return 0;
        }
    }

    if (newBytes.size() < 65536 || newBytes[0] != 'M' || newBytes[1] != 'Z') {
        SetStatus(UPDATE_ERROR, "Скачанный файл повреждён (не является корректной DLL)");
        g_workerRunning = false;
        return 0;
    }

    // Windows allows renaming a currently loaded DLL on disk; rename current RepublicMP.dll -> RepublicMP.dll.old
    std::string oldDll = ownDll + ".old";
    std::string tmpDll = ownDll + ".new";

    FILE* out = nullptr;
    if (fopen_s(&out, tmpDll.c_str(), "wb") != 0 || !out) {
        SetStatus(UPDATE_ERROR, "Не удалось создать временный файл обновления");
        g_workerRunning = false;
        return 0;
    }
    fwrite(newBytes.data(), 1, newBytes.size(), out);
    fclose(out);

    DeleteFileA(oldDll.c_str());
    if (!MoveFileExA(ownDll.c_str(), oldDll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        SetStatus(UPDATE_ERROR, "Не удалось переименовать текущую RepublicMP.dll");
        g_workerRunning = false;
        return 0;
    }
    if (!MoveFileExA(tmpDll.c_str(), ownDll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Restore backup
        MoveFileExA(oldDll.c_str(), ownDll.c_str(), MOVEFILE_REPLACE_EXISTING);
        SetStatus(UPDATE_ERROR, "Не удалось записать новую RepublicMP.dll");
        g_workerRunning = false;
        return 0;
    }

    SaveConfig();
    SetStatus(UPDATE_APPLIED_RESTART_NEEDED, "Обновление установлено! Перезапустите игру (START-COOP.bat) для применения.");
    UiLog_Add("[Updater] RepublicMP.dll успешно обновлена! Перезапустите игру.");
    g_workerRunning = false;
    return 0;
}

void Updater_Bind(const TsmHost* host)
{
    TsmBind(host);
}

void Updater_Init(void)
{
    EnsureLock();
    LoadConfig();
    SaveConfig();
    // Clean up any leftover .old file from a previous in-game update
    std::string ownDll = GetOwnDllPath();
    if (!ownDll.empty()) {
        std::string oldDll = ownDll + ".old";
        DeleteFileA(oldDll.c_str());
    }
    Updater_CheckNow();
}

void Updater_CheckNow(void)
{
    EnsureLock();
    if (g_workerRunning) return;
    g_workerRunning = true;
    HANDLE h = CreateThread(nullptr, 0, CheckThreadProc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else g_workerRunning = false;
}

void Updater_DownloadAndApply(void)
{
    EnsureLock();
    if (g_workerRunning) return;
    g_workerRunning = true;
    HANDLE h = CreateThread(nullptr, 0, ApplyThreadProc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else g_workerRunning = false;
}

UpdateStatus Updater_GetStatus(void)
{
    return g_status;
}

const char* Updater_GetLatestVersion(void)
{
    return g_latestVersion;
}

const char* Updater_GetStatusMessage(void)
{
    return g_statusMessage;
}

const char* Updater_GetGithubRepo(void)
{
    return g_githubRepo;
}

void Updater_SetGithubRepo(const char* repo)
{
    EnsureLock();
    EnterCriticalSection(&g_updLock);
    strncpy_s(g_githubRepo, repo ? repo : "", _TRUNCATE);
    LeaveCriticalSection(&g_updLock);
    SaveConfig();
}
