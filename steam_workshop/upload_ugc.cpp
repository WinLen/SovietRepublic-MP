// SPDX-License-Identifier: GPL-3.0-only
// Direct Steam Workshop publisher for RepublicMP (AppID 784150) using the
// running local Steam client via steam_api64.dll (ISteamUGC).
// No password or Steam Guard prompt required when Steam.exe is running!

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../TesmioLoader/RepublicMP/core/version.h"

#pragma pack(push, 8)
struct CreateItemResult_t {
    int32_t  m_eResult;
    uint64_t m_nPublishedFileId;
    bool     m_bUserNeedsToAcceptWorkshopLegalAgreement;
};

struct SubmitItemUpdateResult_t {
    int32_t  m_eResult;
    bool     m_bUserNeedsToAcceptWorkshopLegalAgreement;
    uint64_t m_nPublishedFileId;
};

struct DeleteItemResult_t {
    int32_t  m_eResult;
    uint64_t m_nPublishedFileId;
};

struct SteamUGCQueryCompleted_t {
    uint64_t m_handle;
    int32_t  m_eResult;
    uint32_t m_unNumResultsReturned;
    uint32_t m_unTotalMatchingResults;
    bool     m_bCachedData;
    char     m_rgchNextCursor[256];
};

struct SteamUGCDetails_t {
    uint64_t m_nPublishedFileId;
    int32_t  m_eResult;
    int32_t  m_eFileType;
    uint32_t m_nCreatorAppID;
    uint32_t m_nConsumerAppID;
    char     m_rgchTitle[129];
    char     m_rgchDescription[8000];
    uint64_t m_ulSteamIDOwner;
    uint32_t m_rtimeCreated;
    uint32_t m_rtimeUpdated;
    uint32_t m_rtimeAddedToUserList;
    int32_t  m_eVisibility;
    bool     m_bBanned;
    bool     m_bAcceptedForUse;
    bool     m_bTagsTruncated;
    char     m_rgchTags[1025];
    uint64_t m_hFile;
    uint64_t m_hPreviewFile;
    char     m_pchFileName[260];
    int32_t  m_nFileSize;
    int32_t  m_nPreviewFileSize;
    char     m_rgchURL[256];
    uint32_t m_unVotesUp;
    uint32_t m_unVotesDown;
    float    m_flScore;
    uint32_t m_unNumChildren;
    uint64_t m_ulTotalFilesSize;
    char     m_pad[4096];
};
#pragma pack(pop)

typedef int32_t HSteamUser;
typedef bool (*Fn_SteamAPI_Init)();
typedef void (*Fn_SteamAPI_Shutdown)();
typedef void (*Fn_SteamAPI_RunCallbacks)();
typedef HSteamUser (*Fn_SteamAPI_GetHSteamUser)();
typedef void* (*Fn_SteamInternal_FindOrCreateUserInterface)(HSteamUser hSteamUser, const char* pszVersion);

typedef uint64_t (*Fn_ISteamUGC_CreateItem)(void* self, uint32_t nConsumerAppId, int32_t eFileType);
typedef uint64_t (*Fn_ISteamUGC_StartItemUpdate)(void* self, uint32_t nConsumerAppId, uint64_t nPublishedFileID);
typedef bool (*Fn_ISteamUGC_SetItemTitle)(void* self, uint64_t handle, const char* pchTitle);
typedef bool (*Fn_ISteamUGC_SetItemDescription)(void* self, uint64_t handle, const char* pchDescription);
typedef bool (*Fn_ISteamUGC_SetItemUpdateLanguage)(void* self, uint64_t handle, const char* pchLanguage);
typedef bool (*Fn_ISteamUGC_SetItemVisibility)(void* self, uint64_t handle, int32_t eVisibility);
typedef bool (*Fn_ISteamUGC_SetItemContent)(void* self, uint64_t handle, const char* pszContentFolder);
typedef bool (*Fn_ISteamUGC_SetItemPreview)(void* self, uint64_t handle, const char* pszPreviewFile);
typedef uint64_t (*Fn_ISteamUGC_SubmitItemUpdate)(void* self, uint64_t handle, const char* pchChangeNote);
typedef int32_t (*Fn_ISteamUGC_GetItemUpdateProgress)(void* self, uint64_t handle, uint64_t* punBytesProcessed, uint64_t* punBytesTotal);
typedef uint64_t (*Fn_ISteamUGC_DeleteItem)(void* self, uint64_t nPublishedFileID);
typedef uint64_t (*Fn_ISteamUGC_SubscribeItem)(void* self, uint64_t nPublishedFileID);
typedef bool (*Fn_ISteamUGC_DownloadItem)(void* self, uint64_t nPublishedFileID, bool bHighPriority);
typedef uint64_t (*Fn_ISteamUGC_CreateQueryUGCDetailsRequest)(void* self, uint64_t* pvecPublishedFileID, uint32_t unNumPublishedFileIDs);
typedef bool (*Fn_ISteamUGC_SetLanguage)(void* self, uint64_t handle, const char* pchLanguage);
typedef bool (*Fn_ISteamUGC_SetAllowCachedResponse)(void* self, uint64_t handle, uint32_t unMaxAgeSeconds);
typedef uint64_t (*Fn_ISteamUGC_SendQueryUGCRequest)(void* self, uint64_t handle);
typedef bool (*Fn_ISteamUGC_GetQueryUGCResult)(void* self, uint64_t handle, uint32_t index, SteamUGCDetails_t* pDetails);
typedef bool (*Fn_ISteamUGC_ReleaseQueryUGCRequest)(void* self, uint64_t handle);

typedef void (*CreateItemCallbackFn)(CreateItemResult_t* pParam, bool bIOFailure);
typedef void (*SubmitItemCallbackFn)(SubmitItemUpdateResult_t* pParam, bool bIOFailure);
typedef void (*DeleteItemCallbackFn)(DeleteItemResult_t* pParam, bool bIOFailure);
typedef void (*QueryCompletedCallbackFn)(SteamUGCQueryCompleted_t* pParam, bool bIOFailure);
typedef void* (*Fn_CCreateItemResult_t_SetCallResult)(uint64_t hAPICall, CreateItemCallbackFn fn);
typedef void* (*Fn_CSubmitItemUpdateResult_t_SetCallResult)(uint64_t hAPICall, SubmitItemCallbackFn fn);
typedef void* (*Fn_CDeleteItemResult_t_SetCallResult)(uint64_t hAPICall, DeleteItemCallbackFn fn);
typedef void* (*Fn_CSteamUGCQueryCompleted_t_SetCallResult)(uint64_t hAPICall, QueryCompletedCallbackFn fn);

static volatile bool g_createDone = false;
static CreateItemResult_t g_createResult = {};
static volatile bool g_createIOFail = false;

static volatile bool g_submitDone = false;
static SubmitItemUpdateResult_t g_submitResult = {};
static volatile bool g_submitIOFail = false;

static volatile bool g_deleteDone = false;
static DeleteItemResult_t g_deleteResult = {};
static volatile bool g_deleteIOFail = false;

static volatile bool g_queryDone = false;
static SteamUGCQueryCompleted_t g_queryResult = {};
static volatile bool g_queryIOFail = false;

static void OnQueryCompleted(SteamUGCQueryCompleted_t* pParam, bool bIOFailure)
{
    if (pParam) g_queryResult = *pParam;
    g_queryIOFail = bIOFailure;
    g_queryDone = true;
}

static void OnCreateItem(CreateItemResult_t* pParam, bool bIOFailure)
{
    if (pParam) g_createResult = *pParam;
    g_createIOFail = bIOFailure;
    g_createDone = true;
}

static void OnDeleteItem(DeleteItemResult_t* pParam, bool bIOFailure)
{
    if (pParam) g_deleteResult = *pParam;
    g_deleteIOFail = bIOFailure;
    g_deleteDone = true;
}

static void OnSubmitItem(SubmitItemUpdateResult_t* pParam, bool bIOFailure)
{
    if (pParam) g_submitResult = *pParam;
    g_submitIOFail = bIOFailure;
    g_submitDone = true;
}

static std::string ReadFileUtf8(const char* path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return ""; }
    std::string s((size_t)sz, '\0');
    fread(&s[0], 1, (size_t)sz, f);
    fclose(f);
    return s;
}

static bool SafeReplaceDll(const std::string& srcPath, const std::string& dstPath)
{
    if (GetFileAttributesA(srcPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    if (CopyFileA(srcPath.c_str(), dstPath.c_str(), FALSE)) return true;
    // If locked by running SOVIET64.exe, rename locked file to .old first
    std::string oldPath = dstPath + ".old";
    DeleteFileA(oldPath.c_str());
    MoveFileExA(dstPath.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    return CopyFileA(srcPath.c_str(), dstPath.c_str(), FALSE) != 0;
}

static void SyncLocalWorkshopFolder(const std::string& lws,
                                    const std::string& contentDir,
                                    const std::string& srcRepublicMp,
                                    const std::string& srcLoader,
                                    const std::string& srcLauncher,
                                    const std::string& srcDinput8)
{
    if (GetFileAttributesA(lws.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA((lws + "\\RepublicMP.dll").c_str());
    DeleteFileA((lws + "\\update_mod.ps1").c_str());
    DeleteFileA((lws + "\\CREATE-DESKTOP-SHORTCUT.bat").c_str());
    DeleteFileA((lws + "\\config.ini").c_str());
    DeleteFileA((lws + "\\log.html").c_str());
    DeleteFileA((lws + "\\media_soviet\\config.ini").c_str());
    CreateDirectoryA((lws + "\\TesmioLoader").c_str(), nullptr);
    CreateDirectoryA((lws + "\\TesmioLoader\\build").c_str(), nullptr);
    CreateDirectoryA((lws + "\\TesmioLoader\\build\\plugins").c_str(), nullptr);
    SafeReplaceDll(srcRepublicMp, lws + "\\TesmioLoader\\build\\plugins\\RepublicMP.dll");
    SafeReplaceDll(srcLoader,     lws + "\\TesmioLoader\\build\\tesmioloader.dll");
    SafeReplaceDll(srcLauncher,   lws + "\\TesmioLoader\\build\\tesmiolauncher.exe");
    SafeReplaceDll(srcDinput8,    lws + "\\dinput8.dll");
    SafeReplaceDll(contentDir + "\\START-COOP.bat", lws + "\\START-COOP.bat");
    SafeReplaceDll(contentDir + "\\README-RU.txt",  lws + "\\README-RU.txt");
    SafeReplaceDll(contentDir + "\\LICENSE",        lws + "\\LICENSE");
    std::string localIni = lws + "\\TesmioLoader\\build\\plugins\\republicmp_updater.ini";
    WritePrivateProfileStringA("updater", "github_repo", REPUBLICMP_GITHUB_REPO, localIni.c_str());
    WritePrivateProfileStringA("updater", "current_version", REPUBLICMP_VERSION, localIni.c_str());
    printf("[UGC] Synced latest v%s DLLs & scripts into local Steam Workshop folder (%s)\n", REPUBLICMP_VERSION, lws.c_str());
}

static void SyncLatestModBinaries(const std::string& repoRoot, const std::string& wsDir, const std::string& contentDir, uint64_t activeId)
{
    std::string buildDir = repoRoot + "\\TesmioLoader\\build";
    std::string srcRepublicMp = buildDir + "\\plugins\\RepublicMP.dll";
    std::string srcLoader     = buildDir + "\\tesmioloader.dll";
    std::string srcLauncher   = buildDir + "\\tesmiolauncher.exe";
    std::string srcDinput8    = buildDir + "\\dinput8.dll";

    // Remove dead duplicate DLLs and obsolete updater scripts from root folders
    DeleteFileA((wsDir + "\\RepublicMP.dll").c_str());
    DeleteFileA((contentDir + "\\RepublicMP.dll").c_str());
    DeleteFileA((contentDir + "\\update_mod.ps1").c_str());
    DeleteFileA((contentDir + "\\CREATE-DESKTOP-SHORTCUT.bat").c_str());

    // 1. Always stage fresh binaries into steam_workshop\content (uploaded to Steam Workshop)
    CreateDirectoryA((contentDir + "\\TesmioLoader").c_str(), nullptr);
    CreateDirectoryA((contentDir + "\\TesmioLoader\\build").c_str(), nullptr);
    CreateDirectoryA((contentDir + "\\TesmioLoader\\build\\plugins").c_str(), nullptr);

    SafeReplaceDll(srcRepublicMp, contentDir + "\\TesmioLoader\\build\\plugins\\RepublicMP.dll");
    SafeReplaceDll(srcLoader,     contentDir + "\\TesmioLoader\\build\\tesmioloader.dll");
    SafeReplaceDll(srcLauncher,   contentDir + "\\TesmioLoader\\build\\tesmiolauncher.exe");
    SafeReplaceDll(srcDinput8,    contentDir + "\\dinput8.dll");

    std::string iniPath = contentDir + "\\TesmioLoader\\build\\plugins\\republicmp_updater.ini";
    WritePrivateProfileStringA("updater", "github_repo", REPUBLICMP_GITHUB_REPO, iniPath.c_str());
    WritePrivateProfileStringA("updater", "current_version", REPUBLICMP_VERSION, iniPath.c_str());
    printf("[UGC] Synced latest v%s DLLs into %s\n", REPUBLICMP_VERSION, contentDir.c_str());

    // 2. Also sync into local subscribed Steam Workshop folders (3823543010 & 3834234182) and Steam game dir if present
    SyncLocalWorkshopFolder("C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\784150\\3823543010",
                            contentDir, srcRepublicMp, srcLoader, srcLauncher, srcDinput8);
    SyncLocalWorkshopFolder("C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\784150\\3834234182",
                            contentDir, srcRepublicMp, srcLoader, srcLauncher, srcDinput8);
    if (activeId > 0 && activeId != 3823543010ULL && activeId != 3834234182ULL) {
        char activeDir[MAX_PATH] = {};
        _snprintf_s(activeDir, sizeof(activeDir), _TRUNCATE,
                    "C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\784150\\%llu",
                    (unsigned long long)activeId);
        SyncLocalWorkshopFolder(activeDir, contentDir, srcRepublicMp, srcLoader, srcLauncher, srcDinput8);
    }

    const char* steamGameDir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic";
    if (GetFileAttributesA(steamGameDir) != INVALID_FILE_ATTRIBUTES) {
        SafeReplaceDll(srcDinput8, std::string(steamGameDir) + "\\dinput8.dll");
    }
}

static void QueryWorkshopLanguage(
    void* pUGC, uint64_t qId, const char* lang,
    Fn_SteamAPI_RunCallbacks pRunCallbacks,
    Fn_ISteamUGC_CreateQueryUGCDetailsRequest pCreateQuery,
    Fn_ISteamUGC_SetLanguage pSetQueryLang,
    Fn_ISteamUGC_SetAllowCachedResponse pSetAllowCached,
    Fn_ISteamUGC_SendQueryUGCRequest pSendQuery,
    Fn_ISteamUGC_GetQueryUGCResult pGetQueryRes,
    Fn_ISteamUGC_ReleaseQueryUGCRequest pReleaseQuery,
    Fn_CSteamUGCQueryCompleted_t_SetCallResult pSetQueryCall)
{
    uint64_t hQuery = pCreateQuery(pUGC, &qId, 1);
    if (pSetAllowCached) pSetAllowCached(pUGC, hQuery, 0);
    if (pSetQueryLang && lang) pSetQueryLang(pUGC, hQuery, lang);
    g_queryDone = false;
    g_queryIOFail = false;
    memset(&g_queryResult, 0, sizeof(g_queryResult));
    uint64_t hCall = pSendQuery(pUGC, hQuery);
    pSetQueryCall(hCall, OnQueryCompleted);
    while (!g_queryDone) {
        pRunCallbacks();
        Sleep(50);
    }
    printf("[UGC] Query(%llu, %s): queryResult=%d numResults=%u cached=%d ioFail=%d\n",
           (unsigned long long)qId, lang ? lang : "default",
           (int)g_queryResult.m_eResult, g_queryResult.m_unNumResultsReturned,
           (int)g_queryResult.m_bCachedData, (int)g_queryIOFail);
    if (g_queryResult.m_unNumResultsReturned > 0) {
        static SteamUGCDetails_t det = {};
        memset(&det, 0, sizeof(det));
        if (pGetQueryRes(pUGC, hQuery, 0, &det)) {
            printf("  [%s] itemResult=%d fileType=%d creatorApp=%u consumerApp=%u\n",
                   lang ? lang : "default",
                   (int)det.m_eResult, (int)det.m_eFileType, det.m_nCreatorAppID, det.m_nConsumerAppID);
            printf("  [%s] title='%s'\n", lang ? lang : "default", det.m_rgchTitle);
            printf("  [%s] owner=%llu created=%u updated=%u vis=%d banned=%d accepted=%d fileSize=%d prevSize=%d\n",
                   lang ? lang : "default",
                   (unsigned long long)det.m_ulSteamIDOwner, det.m_rtimeCreated, det.m_rtimeUpdated,
                   (int)det.m_eVisibility, (int)det.m_bBanned, (int)det.m_bAcceptedForUse,
                   (int)det.m_nFileSize, (int)det.m_nPreviewFileSize);
            printf("  [%s] desc='%.120s'\n", lang ? lang : "default", det.m_rgchDescription);
        }
    }
    pReleaseQuery(pUGC, hQuery);
}

static bool SubmitAndWait(
    void* pUGC, uint64_t hUpdate, const char* changeNote,
    Fn_ISteamUGC_SubmitItemUpdate pSubmitUpdate,
    Fn_CSubmitItemUpdateResult_t_SetCallResult pSetSubmitCall,
    Fn_ISteamUGC_GetItemUpdateProgress pGetProgress,
    Fn_SteamAPI_RunCallbacks pRunCallbacks)
{
    g_submitDone = false;
    g_submitIOFail = false;
    memset(&g_submitResult, 0, sizeof(g_submitResult));
    uint64_t hSubmitCall = pSubmitUpdate(pUGC, hUpdate, changeNote);
    pSetSubmitCall(hSubmitCall, OnSubmitItem);

    int32_t lastStatus = -1;
    while (!g_submitDone) {
        pRunCallbacks();
        uint64_t processed = 0, total = 0;
        int32_t st = pGetProgress(pUGC, hUpdate, &processed, &total);
        if (st != lastStatus || total > 0) {
            printf("\r[UGC] Upload progress: status=%d (%llu / %llu bytes)   ", (int)st,
                   (unsigned long long)processed, (unsigned long long)total);
            fflush(stdout);
            lastStatus = st;
        }
        Sleep(100);
    }
    printf("\n");
    return !g_submitIOFail && g_submitResult.m_eResult == 1;
}

int main(int argc, char** argv)
{
    SetEnvironmentVariableA("SteamAppId", "784150");
    SetEnvironmentVariableA("SteamGameId", "784150");

    // Write steam_appid.txt in current dir just in case
    FILE* fa = nullptr;
    if (fopen_s(&fa, "steam_appid.txt", "wb") == 0 && fa) {
        fwrite("784150", 1, 6, fa);
        fclose(fa);
    }

    const char* dllCandidates[] = {
        "steam_api64.dll",
        "..\\local\\test-host\\steam_api64.dll",
        "local\\test-host\\steam_api64.dll",
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic\\steam_api64.dll"
    };
    HMODULE hDll = nullptr;
    for (const char* c : dllCandidates) {
        hDll = LoadLibraryA(c);
        if (hDll) {
            printf("[UGC] Loaded %s\n", c);
            break;
        }
    }
    if (!hDll) {
        printf("[ERROR] Could not load steam_api64.dll!\n");
        return 1;
    }

    auto pInit = (Fn_SteamAPI_Init)GetProcAddress(hDll, "SteamAPI_Init");
    auto pShutdown = (Fn_SteamAPI_Shutdown)GetProcAddress(hDll, "SteamAPI_Shutdown");
    auto pRunCallbacks = (Fn_SteamAPI_RunCallbacks)GetProcAddress(hDll, "SteamAPI_RunCallbacks");
    auto pGetHSteamUser = (Fn_SteamAPI_GetHSteamUser)GetProcAddress(hDll, "SteamAPI_GetHSteamUser");
    auto pFindIface = (Fn_SteamInternal_FindOrCreateUserInterface)GetProcAddress(hDll, "SteamInternal_FindOrCreateUserInterface");

    auto pCreateItem = (Fn_ISteamUGC_CreateItem)GetProcAddress(hDll, "SteamAPI_ISteamUGC_CreateItem");
    auto pStartUpdate = (Fn_ISteamUGC_StartItemUpdate)GetProcAddress(hDll, "SteamAPI_ISteamUGC_StartItemUpdate");
    auto pSetTitle = (Fn_ISteamUGC_SetItemTitle)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemTitle");
    auto pSetDesc = (Fn_ISteamUGC_SetItemDescription)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemDescription");
    auto pSetUpdateLang = (Fn_ISteamUGC_SetItemUpdateLanguage)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemUpdateLanguage");
    auto pSetVis = (Fn_ISteamUGC_SetItemVisibility)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemVisibility");
    auto pSetContent = (Fn_ISteamUGC_SetItemContent)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemContent");
    auto pSetPreview = (Fn_ISteamUGC_SetItemPreview)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemPreview");
    auto pSubmitUpdate = (Fn_ISteamUGC_SubmitItemUpdate)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SubmitItemUpdate");
    auto pGetProgress = (Fn_ISteamUGC_GetItemUpdateProgress)GetProcAddress(hDll, "SteamAPI_ISteamUGC_GetItemUpdateProgress");

    auto pSetCreateCall = (Fn_CCreateItemResult_t_SetCallResult)GetProcAddress(hDll, "CCreateItemResult_t_SetCallResult");
    auto pSetSubmitCall = (Fn_CSubmitItemUpdateResult_t_SetCallResult)GetProcAddress(hDll, "CSubmitItemUpdateResult_t_SetCallResult");
    auto pDeleteItem = (Fn_ISteamUGC_DeleteItem)GetProcAddress(hDll, "SteamAPI_ISteamUGC_DeleteItem");
    auto pSetDeleteCall = (Fn_CDeleteItemResult_t_SetCallResult)GetProcAddress(hDll, "CDeleteItemResult_t_SetCallResult");
    auto pSubscribeItem = (Fn_ISteamUGC_SubscribeItem)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SubscribeItem");
    auto pDownloadItem = (Fn_ISteamUGC_DownloadItem)GetProcAddress(hDll, "SteamAPI_ISteamUGC_DownloadItem");
    auto pCreateQuery = (Fn_ISteamUGC_CreateQueryUGCDetailsRequest)GetProcAddress(hDll, "SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest");
    auto pSetQueryLang = (Fn_ISteamUGC_SetLanguage)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetLanguage");
    auto pSetAllowCached = (Fn_ISteamUGC_SetAllowCachedResponse)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetAllowCachedResponse");
    auto pSendQuery = (Fn_ISteamUGC_SendQueryUGCRequest)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SendQueryUGCRequest");
    auto pGetQueryRes = (Fn_ISteamUGC_GetQueryUGCResult)GetProcAddress(hDll, "SteamAPI_ISteamUGC_GetQueryUGCResult");
    auto pReleaseQuery = (Fn_ISteamUGC_ReleaseQueryUGCRequest)GetProcAddress(hDll, "SteamAPI_ISteamUGC_ReleaseQueryUGCRequest");
    auto pSetQueryCall = (Fn_CSteamUGCQueryCompleted_t_SetCallResult)GetProcAddress(hDll, "CSteamUGCQueryCompleted_t_SetCallResult");

    if (!pInit || !pInit()) {
        printf("[ERROR] SteamAPI_Init() failed! Make sure Steam desktop client is running and logged in.\n");
        return 1;
    }

    void* pUGC = pFindIface(pGetHSteamUser(), "STEAMUGC_INTERFACE_VERSION012");
    if (!pUGC) {
        printf("[ERROR] Could not acquire STEAMUGC_INTERFACE_VERSION012!\n");
        return 1;
    }

    if (argc >= 3 && strcmp(argv[1], "--subscribe") == 0) {
        uint64_t subId = _strtoui64(argv[2], nullptr, 10);
        printf("[UGC] Subscribing and downloading Workshop item %llu...\n", (unsigned long long)subId);
        if (pSubscribeItem) pSubscribeItem(pUGC, subId);
        if (pDownloadItem) pDownloadItem(pUGC, subId, true);
        for (int i = 0; i < 60; ++i) {
            pRunCallbacks();
            Sleep(100);
        }
        DeleteFileA("steam_appid.txt");
        if (pShutdown) pShutdown();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "--query") == 0) {
        uint64_t qId = _strtoui64(argv[2], nullptr, 10);
        QueryWorkshopLanguage(pUGC, qId, "english", pRunCallbacks, pCreateQuery,
                              pSetQueryLang, pSetAllowCached, pSendQuery, pGetQueryRes, pReleaseQuery, pSetQueryCall);
        QueryWorkshopLanguage(pUGC, qId, "russian", pRunCallbacks, pCreateQuery,
                              pSetQueryLang, pSetAllowCached, pSendQuery, pGetQueryRes, pReleaseQuery, pSetQueryCall);
        DeleteFileA("steam_appid.txt");
        if (pShutdown) pShutdown();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "--delete") == 0) {
        uint64_t delId = _strtoui64(argv[2], nullptr, 10);
        if (delId == 3823543010ULL) {
            printf("[ERROR] Refusing to delete the main RepublicMP Workshop item 3823543010!\n");
            DeleteFileA("steam_appid.txt");
            if (pShutdown) pShutdown();
            return 1;
        }
        printf("[UGC] Deleting Workshop item %llu...\n", (unsigned long long)delId);
        g_deleteDone = false;
        uint64_t hDelCall = pDeleteItem(pUGC, delId);
        pSetDeleteCall(hDelCall, OnDeleteItem);
        while (!g_deleteDone) {
            pRunCallbacks();
            Sleep(50);
        }
        DeleteFileA("steam_appid.txt");
        if (pShutdown) pShutdown();
        printf("[UGC] DeleteItem(%llu) finished: EResult=%d, IOFail=%d\n",
               (unsigned long long)delId, (int)g_deleteResult.m_eResult, (int)g_deleteIOFail);
        return (g_deleteResult.m_eResult == 1 && !g_deleteIOFail) ? 0 : 1;
    }

    char rootDir[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, rootDir);
    std::string repoRoot(rootDir);
    // Support running upload_ugc.exe either from repo root or from inside steam_workshop directory
    if (repoRoot.size() > 15 && _stricmp(repoRoot.c_str() + repoRoot.size() - 15, "\\steam_workshop") == 0) {
        repoRoot.resize(repoRoot.size() - 15);
    }
    std::string wsDir = repoRoot + "\\steam_workshop";
    std::string idFile = wsDir + "\\published_file_id.txt";
    std::string contentDir = wsDir + "\\content";
    std::string previewFile = wsDir + "\\preview.png";
    std::string descFile = wsDir + "\\description_ru.txt";

    bool createFriendsItem = false;
    bool updateMeta = false;
    bool updatePreview = false;
    bool setVis = false;
    int32_t targetVis = 0;
    uint64_t argPublishedId = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--create-friends") == 0) {
            createFriendsItem = true;
            updateMeta = true;
            updatePreview = true;
            setVis = true;
            targetVis = 1;
        } else if (strcmp(argv[i], "--meta") == 0) {
            updateMeta = true;
        } else if (strcmp(argv[i], "--preview") == 0) {
            updatePreview = true;
        } else if (strcmp(argv[i], "--public") == 0) {
            setVis = true;
            targetVis = 0;
        } else if (strcmp(argv[i], "--friends") == 0) {
            setVis = true;
            targetVis = 1;
        } else if (argv[i][0] != '-') {
            uint64_t parsedArg = _strtoui64(argv[i], nullptr, 10);
            if (parsedArg > 0) argPublishedId = parsedArg;
        }
    }

    uint64_t publishedId = 3823543010ULL;

    if (createFriendsItem) {
        printf("[UGC] Creating new Friends-Only Steam Workshop item for AppID 784150...\n");
        g_createDone = false;
        uint64_t hCreateCall = pCreateItem(pUGC, 784150, 0 /* k_EWorkshopFileTypeCommunity */);
        pSetCreateCall(hCreateCall, OnCreateItem);
        while (!g_createDone) {
            pRunCallbacks();
            Sleep(50);
        }
        if (g_createIOFail || g_createResult.m_eResult != 1 || g_createResult.m_nPublishedFileId == 0) {
            DeleteFileA("steam_appid.txt");
            if (pShutdown) pShutdown();
            printf("[ERROR] CreateItem failed! EResult=%d, IOFail=%d\n",
                   (int)g_createResult.m_eResult, (int)g_createIOFail);
            return 1;
        }
        publishedId = g_createResult.m_nPublishedFileId;
        FILE* fid = nullptr;
        if (fopen_s(&fid, idFile.c_str(), "wb") == 0 && fid) {
            char idBuf[64] = {};
            int len = _snprintf_s(idBuf, sizeof(idBuf), _TRUNCATE, "%llu\n", (unsigned long long)publishedId);
            if (len > 0) fwrite(idBuf, 1, (size_t)len, fid);
            fclose(fid);
        }
        printf("[UGC] Created new Workshop item ID: %llu (saved to %s)\n",
               (unsigned long long)publishedId, idFile.c_str());
    } else {
        std::string existingIdStr = ReadFileUtf8(idFile.c_str());
        if (!existingIdStr.empty()) {
            uint64_t parsedId = _strtoui64(existingIdStr.c_str(), nullptr, 10);
            if (parsedId > 0) publishedId = parsedId;
        }
        if (argPublishedId > 0) {
            publishedId = argPublishedId;
        }
    }

    SyncLatestModBinaries(repoRoot, wsDir, contentDir, publishedId);

    printf("[UGC] Updating Steam Workshop item: %llu (updateMeta=%d updatePreview=%d setVis=%d)\n",
           (unsigned long long)publishedId, (int)updateMeta, (int)updatePreview, (int)setVis);

    const bool isFriendsMirror = createFriendsItem || (publishedId == 3834234182ULL);
    std::string titleToSet;
    std::string descToSet;
    if (updateMeta) {
        if (isFriendsMirror) {
            titleToSet = "RepublicMP v" REPUBLICMP_VERSION " - Co-op Multiplayer";
            descToSet =
                "RepublicMP v" REPUBLICMP_VERSION " - Co-op Multiplayer (Friends-Only).\n"
                "- Lockstep + 1-click Steam P2P\n"
                "- Full LAN auto-discovery (Radmin VPN / Hamachi / ZeroTier) & Steam-free offline mode\n"
                "- One State / Two States on One Map\n"
                "- Automatic Steam Workshop Mod Sync & DLC Guard";
        } else {
            titleToSet = "RepublicMP v" REPUBLICMP_VERSION " - Кооператив мультиплеер 1 или 2 государства на одной карте";
            descToSet = ReadFileUtf8(descFile.c_str());
            if (descToSet.empty() || descToSet.size() >= 8000) {
                printf("[ERROR] Invalid %s length (%zu bytes; must be 1..7999 bytes for Steamworks ISteamUGC)!\n",
                       descFile.c_str(), descToSet.size());
                DeleteFileA("steam_appid.txt");
                if (pShutdown) pShutdown();
                return 1;
            }
        }
    }

    uint64_t hUpdate = pStartUpdate(pUGC, 784150, publishedId);
    bool bLangEn = (updateMeta && pSetUpdateLang) ? pSetUpdateLang(pUGC, hUpdate, "english") : true;
    bool bTitle = updateMeta ? pSetTitle(pUGC, hUpdate, titleToSet.c_str()) : true;
    bool bDesc = updateMeta ? pSetDesc(pUGC, hUpdate, descToSet.c_str()) : true;
    // Only call SetItemVisibility when explicitly requested (--public / --friends / --create-friends)
    // because Steam returns k_EResultAccessDenied (15) on SetItemVisibility during automated content scans.
    bool bVis = setVis ? pSetVis(pUGC, hUpdate, targetVis) : true;
    bool bContent = pSetContent(pUGC, hUpdate, contentDir.c_str());
    bool bPreview = updatePreview ? pSetPreview(pUGC, hUpdate, previewFile.c_str()) : true;
    printf("[UGC] SetItem[en] (id=%llu): lang=%d title=%d desc=%d (%zu bytes) vis=%d content=%d preview=%d (uploadPreview=%d)\n",
           (unsigned long long)publishedId, (int)bLangEn, (int)bTitle, (int)bDesc,
           updateMeta ? descToSet.size() : (size_t)0,
           (int)bVis, (int)bContent, (int)bPreview, (int)updatePreview);

    char defaultNote[512];
    _snprintf_s(defaultNote, sizeof(defaultNote), _TRUNCATE,
                "v%s (Protocol %d) - Fixed POWER_HIGH/POWER_LOW/PIPE/WATERPIPE desync & replay: deduplicated and completed sim_patches kToolCalls to all 186 Late tool sites (18 missing variants incl. eletric_high2/4-7/uhigh2, pipe/waterpipe, roads, HoverTools_Junction), fixed swapped road_sync category wrappers, and added Commands_TakeUi() defense-in-depth in HookedRoadCommit; 74/74 tests + MSVC /analyze + clang-tidy + ub-mode clean",
                REPUBLICMP_VERSION, (int)REPUBLICMP_PROTOCOL);

    printf("[UGC] Submitting content (%s)...\n", contentDir.c_str());
    if (!SubmitAndWait(pUGC, hUpdate, defaultNote, pSubmitUpdate, pSetSubmitCall, pGetProgress, pRunCallbacks)) {
        DeleteFileA("steam_appid.txt");
        if (pShutdown) pShutdown();
        printf("[ERROR] SubmitItemUpdate failed! EResult=%d, IOFail=%d\n",
               (int)g_submitResult.m_eResult, (int)g_submitIOFail);
        return 1;
    }

    if (updateMeta && pSetUpdateLang) {
        printf("[UGC] Syncing Russian language metadata slot for %llu...\n", (unsigned long long)publishedId);
        uint64_t hUpdateRu = pStartUpdate(pUGC, 784150, publishedId);
        bool bLangRu = pSetUpdateLang(pUGC, hUpdateRu, "russian");
        bool bTitleRu = pSetTitle(pUGC, hUpdateRu, titleToSet.c_str());
        bool bDescRu = pSetDesc(pUGC, hUpdateRu, descToSet.c_str());
        printf("[UGC] SetItem[ru] (id=%llu): lang=%d title=%d desc=%d (%zu bytes)\n",
               (unsigned long long)publishedId, (int)bLangRu, (int)bTitleRu, (int)bDescRu, descToSet.size());
        if (!SubmitAndWait(pUGC, hUpdateRu, nullptr, pSubmitUpdate, pSetSubmitCall, pGetProgress, pRunCallbacks)) {
            DeleteFileA("steam_appid.txt");
            if (pShutdown) pShutdown();
            printf("[ERROR] SubmitItemUpdate[ru] failed! EResult=%d, IOFail=%d\n",
                   (int)g_submitResult.m_eResult, (int)g_submitIOFail);
            return 1;
        }
    }

    DeleteFileA("steam_appid.txt");
    if (pShutdown) pShutdown();

    printf("[SUCCESS] Published to Steam Workshop (%llu)!\n", (unsigned long long)publishedId);
    printf("Workshop URL: https://steamcommunity.com/sharedfiles/filedetails/?id=%llu\n",
           (unsigned long long)publishedId);
    return 0;
}
