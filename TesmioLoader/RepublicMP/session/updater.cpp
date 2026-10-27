// SPDX-License-Identifier: GPL-3.0-only
// See updater.h.
#include "updater.h"
#include "../core/version.h"
#include "../core/safe_mem.h"
#include "../ui/ui_log.h"
#include "../ui/lang.h"
#include "../third_party/miniz/miniz.h"

#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")

static INIT_ONCE g_updLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_updLock;
static volatile UpdateStatus g_status = UPDATE_IDLE;
static char g_latestVersion[64] = "";
static char g_statusMessage[256] = "";
static char g_githubRepo[128] = REPUBLICMP_GITHUB_REPO;
static char g_downloadUrl[512] = "";
static char g_workshopDllPath[MAX_PATH] = "";
static volatile bool g_workerRunning = false;

static void EnsureLock()
{
    TsmEnsureCriticalSection(&g_updLockOnce, &g_updLock);
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

static bool IsRemoteVersionNewer(const std::string& remoteVer, const std::string& localVer)
{
    int rMaj = 0, rMin = 0, rPat = 0;
    int lMaj = 0, lMin = 0, lPat = 0;
    if (sscanf_s(remoteVer.c_str(), "%d.%d.%d", &rMaj, &rMin, &rPat) < 2) return false;
    if (sscanf_s(localVer.c_str(), "%d.%d.%d", &lMaj, &lMin, &lPat) < 2) return false;
    if (rMaj != lMaj) return rMaj > lMaj;
    if (rMin != lMin) return rMin > lMin;
    return rPat > lPat;
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
    SetStatus(UPDATE_CHECKING, Tr("Проверка обновлений...", "Checking for updates..."));

    // 1. Check Steam Workshop folder first
    std::string wsDll;
    if (CheckWorkshopNewerDll(wsDll)) {
        EnterCriticalSection(&g_updLock);
        strncpy_s(g_workshopDllPath, wsDll.c_str(), _TRUNCATE);
        strncpy_s(g_latestVersion, "Steam Workshop", _TRUNCATE);
        LeaveCriticalSection(&g_updLock);
        SetStatus(UPDATE_AVAILABLE_WORKSHOP, Tr("Найдена новая версия в папке Steam Workshop!", "New version found in Steam Workshop folder!"));
        UiLog_Add(Tr("[Updater] Найдена обновлённая сборка в Steam Workshop", "[Updater] Newer build found in Steam Workshop"));
        g_workerRunning = false;
        return 0;
    }

    // 2. Check GitHub Releases if repo is configured
    char repo[128] = {};
    EnterCriticalSection(&g_updLock);
    strncpy_s(repo, g_githubRepo, _TRUNCATE);
    LeaveCriticalSection(&g_updLock);

    if (!repo[0]) {
        SetStatus(UPDATE_IDLE, Tr("Репозиторий GitHub не задан (укажите owner/repo для автообновления)", "GitHub repo not configured (set owner/repo for auto-update)"));
        g_workerRunning = false;
        return 0;
    }

    char apiUrl[256];
    _snprintf_s(apiUrl, sizeof(apiUrl), _TRUNCATE, "https://api.github.com/repos/%s/releases/latest", repo);

    std::vector<unsigned char> body;
    if (!HttpGetBytes(apiUrl, body)) {
        SetStatus(UPDATE_ERROR, Tr("Не удалось получить релизы с GitHub (проверьте owner/repo или интернет)", "Failed to fetch GitHub releases (check owner/repo or internet)"));
        g_workerRunning = false;
        return 0;
    }

    std::string json((const char*)body.data(), body.size());
    std::string tag = ExtractJsonString(json, "tag_name");
    if (tag.empty()) {
        SetStatus(UPDATE_ERROR, Tr("Ответ GitHub API не содержит tag_name", "GitHub API response missing tag_name"));
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

    if (IsRemoteVersionNewer(normRemote, normLocal) && !dllUrl.empty()) {
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    Tr("Доступно обновление на GitHub: %s (текущая v%s)", "Update available on GitHub: %s (current v%s)"),
                    tag.c_str(), REPUBLICMP_VERSION);
        SetStatus(UPDATE_AVAILABLE_GITHUB, msg);
        UiLog_Add(msg);
    } else {
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    Tr("Установлена актуальная версия (v%s)", "Up to date (v%s)"),
                    REPUBLICMP_VERSION);
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
        SetStatus(UPDATE_ERROR, Tr("Не удалось определить путь к RepublicMP.dll", "Failed to determine path to RepublicMP.dll"));
        g_workerRunning = false;
        return 0;
    }

    std::vector<unsigned char> newBytes;

    if (curStatus == UPDATE_AVAILABLE_WORKSHOP) {
        SetStatus(UPDATE_DOWNLOADING, Tr("Копирование обновления из Steam Workshop...", "Copying update from Steam Workshop..."));
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
        SetStatus(UPDATE_DOWNLOADING, Tr("Скачивание RepublicMP.dll с GitHub Releases...", "Downloading RepublicMP.dll from GitHub Releases..."));
        char url[512] = {};
        EnterCriticalSection(&g_updLock);
        strncpy_s(url, g_downloadUrl, _TRUNCATE);
        LeaveCriticalSection(&g_updLock);

        if (!url[0] || !HttpGetBytes(url, newBytes)) {
            SetStatus(UPDATE_ERROR, Tr("Ошибка при скачивании RepublicMP.dll с GitHub", "Failed to download RepublicMP.dll from GitHub"));
            g_workerRunning = false;
            return 0;
        }
    }

    if (newBytes.size() < 65536 || newBytes[0] != 'M' || newBytes[1] != 'Z') {
        SetStatus(UPDATE_ERROR, Tr("Скачанный файл повреждён (не является корректной DLL)", "Downloaded file is corrupted (not a valid DLL)"));
        g_workerRunning = false;
        return 0;
    }

    // Windows allows renaming a currently loaded DLL on disk; rename current RepublicMP.dll -> RepublicMP.dll.old
    std::string oldDll = ownDll + ".old";
    std::string tmpDll = ownDll + ".new";

    FILE* out = nullptr;
    if (fopen_s(&out, tmpDll.c_str(), "wb") != 0 || !out) {
        SetStatus(UPDATE_ERROR, Tr("Не удалось создать временный файл обновления", "Failed to create temporary update file"));
        g_workerRunning = false;
        return 0;
    }
    fwrite(newBytes.data(), 1, newBytes.size(), out);
    fclose(out);

    DeleteFileA(oldDll.c_str());
    if (!MoveFileExA(ownDll.c_str(), oldDll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        SetStatus(UPDATE_ERROR, Tr("Не удалось переименовать текущую RepublicMP.dll", "Failed to rename current RepublicMP.dll"));
        g_workerRunning = false;
        return 0;
    }
    if (!MoveFileExA(tmpDll.c_str(), ownDll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Restore backup
        MoveFileExA(oldDll.c_str(), ownDll.c_str(), MOVEFILE_REPLACE_EXISTING);
        SetStatus(UPDATE_ERROR, Tr("Не удалось записать новую RepublicMP.dll", "Failed to write new RepublicMP.dll"));
        g_workerRunning = false;
        return 0;
    }

    SaveConfig();
    SetStatus(UPDATE_APPLIED_RESTART_NEEDED, Tr("Обновление установлено! Перезапустите игру для применения.", "Update installed! Restart the game to apply."));
    UiLog_Add(Tr("[Updater] RepublicMP.dll успешно обновлена! Перезапустите игру.", "[Updater] RepublicMP.dll updated! Please restart the game."));
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

static void StartUpdaterWorker(LPTHREAD_START_ROUTINE proc)
{
    EnsureLock();
    if (g_workerRunning) return;
    g_workerRunning = true;
    HANDLE h = CreateThread(nullptr, 0, proc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else g_workerRunning = false;
}

void Updater_CheckNow(void)
{
    StartUpdaterWorker(CheckThreadProc);
}

void Updater_DownloadAndApply(void)
{
    StartUpdaterWorker(ApplyThreadProc);
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
