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
struct SubmitItemUpdateResult_t {
    int32_t  m_eResult;
    bool     m_bUserNeedsToAcceptWorkshopLegalAgreement;
    uint64_t m_nPublishedFileId;
};

struct DeleteItemResult_t {
    int32_t  m_eResult;
    uint64_t m_nPublishedFileId;
};
#pragma pack(pop)

typedef int32_t HSteamUser;
typedef bool (*Fn_SteamAPI_Init)();
typedef void (*Fn_SteamAPI_Shutdown)();
typedef void (*Fn_SteamAPI_RunCallbacks)();
typedef HSteamUser (*Fn_SteamAPI_GetHSteamUser)();
typedef void* (*Fn_SteamInternal_FindOrCreateUserInterface)(HSteamUser hSteamUser, const char* pszVersion);

typedef uint64_t (*Fn_ISteamUGC_StartItemUpdate)(void* self, uint32_t nConsumerAppId, uint64_t nPublishedFileID);
typedef bool (*Fn_ISteamUGC_SetItemTitle)(void* self, uint64_t handle, const char* pchTitle);
typedef bool (*Fn_ISteamUGC_SetItemDescription)(void* self, uint64_t handle, const char* pchDescription);
typedef bool (*Fn_ISteamUGC_SetItemVisibility)(void* self, uint64_t handle, int32_t eVisibility);
typedef bool (*Fn_ISteamUGC_SetItemContent)(void* self, uint64_t handle, const char* pszContentFolder);
typedef bool (*Fn_ISteamUGC_SetItemPreview)(void* self, uint64_t handle, const char* pszPreviewFile);
typedef uint64_t (*Fn_ISteamUGC_SubmitItemUpdate)(void* self, uint64_t handle, const char* pchChangeNote);
typedef int32_t (*Fn_ISteamUGC_GetItemUpdateProgress)(void* self, uint64_t handle, uint64_t* punBytesProcessed, uint64_t* punBytesTotal);
typedef uint64_t (*Fn_ISteamUGC_DeleteItem)(void* self, uint64_t nPublishedFileID);

typedef void (*SubmitItemCallbackFn)(SubmitItemUpdateResult_t* pParam, bool bIOFailure);
typedef void (*DeleteItemCallbackFn)(DeleteItemResult_t* pParam, bool bIOFailure);
typedef void* (*Fn_CSubmitItemUpdateResult_t_SetCallResult)(uint64_t hAPICall, SubmitItemCallbackFn fn);
typedef void* (*Fn_CDeleteItemResult_t_SetCallResult)(uint64_t hAPICall, DeleteItemCallbackFn fn);

static volatile bool g_submitDone = false;
static SubmitItemUpdateResult_t g_submitResult = {};
static volatile bool g_submitIOFail = false;

static volatile bool g_deleteDone = false;
static DeleteItemResult_t g_deleteResult = {};
static volatile bool g_deleteIOFail = false;

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

static void SyncLatestModBinaries(const std::string& repoRoot, const std::string& wsDir, const std::string& contentDir)
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

    // 2. Also sync into local subscribed Steam Workshop folder (3823543010) and Steam game dir if present
    const char* localWs3823543010 = "C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\784150\\3823543010";
    if (GetFileAttributesA(localWs3823543010) != INVALID_FILE_ATTRIBUTES) {
        std::string lws(localWs3823543010);
        DeleteFileA((lws + "\\RepublicMP.dll").c_str());
        DeleteFileA((lws + "\\update_mod.ps1").c_str());
        DeleteFileA((lws + "\\CREATE-DESKTOP-SHORTCUT.bat").c_str());
        SafeReplaceDll(srcRepublicMp, lws + "\\TesmioLoader\\build\\plugins\\RepublicMP.dll");
        SafeReplaceDll(srcLoader,     lws + "\\TesmioLoader\\build\\tesmioloader.dll");
        SafeReplaceDll(srcLauncher,   lws + "\\TesmioLoader\\build\\tesmiolauncher.exe");
        SafeReplaceDll(srcDinput8,    lws + "\\dinput8.dll");
        std::string localIni = lws + "\\TesmioLoader\\build\\plugins\\republicmp_updater.ini";
        WritePrivateProfileStringA("updater", "github_repo", REPUBLICMP_GITHUB_REPO, localIni.c_str());
        WritePrivateProfileStringA("updater", "current_version", REPUBLICMP_VERSION, localIni.c_str());
        printf("[UGC] Synced latest v%s DLLs into local Steam Workshop folder (%s)\n", REPUBLICMP_VERSION, localWs3823543010);
    }
    const char* steamGameDir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SovietRepublic";
    if (GetFileAttributesA(steamGameDir) != INVALID_FILE_ATTRIBUTES) {
        SafeReplaceDll(srcDinput8, std::string(steamGameDir) + "\\dinput8.dll");
    }
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

    auto pStartUpdate = (Fn_ISteamUGC_StartItemUpdate)GetProcAddress(hDll, "SteamAPI_ISteamUGC_StartItemUpdate");
    auto pSetTitle = (Fn_ISteamUGC_SetItemTitle)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemTitle");
    auto pSetDesc = (Fn_ISteamUGC_SetItemDescription)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemDescription");
    auto pSetVis = (Fn_ISteamUGC_SetItemVisibility)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemVisibility");
    auto pSetContent = (Fn_ISteamUGC_SetItemContent)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemContent");
    auto pSetPreview = (Fn_ISteamUGC_SetItemPreview)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemPreview");
    auto pSubmitUpdate = (Fn_ISteamUGC_SubmitItemUpdate)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SubmitItemUpdate");
    auto pGetProgress = (Fn_ISteamUGC_GetItemUpdateProgress)GetProcAddress(hDll, "SteamAPI_ISteamUGC_GetItemUpdateProgress");

    auto pSetSubmitCall = (Fn_CSubmitItemUpdateResult_t_SetCallResult)GetProcAddress(hDll, "CSubmitItemUpdateResult_t_SetCallResult");
    auto pDeleteItem = (Fn_ISteamUGC_DeleteItem)GetProcAddress(hDll, "SteamAPI_ISteamUGC_DeleteItem");
    auto pSetDeleteCall = (Fn_CDeleteItemResult_t_SetCallResult)GetProcAddress(hDll, "CDeleteItemResult_t_SetCallResult");

    if (!pInit || !pInit()) {
        printf("[ERROR] SteamAPI_Init() failed! Make sure Steam desktop client is running and logged in.\n");
        return 1;
    }

    void* pUGC = pFindIface(pGetHSteamUser(), "STEAMUGC_INTERFACE_VERSION012");
    if (!pUGC) {
        printf("[ERROR] Could not acquire STEAMUGC_INTERFACE_VERSION012!\n");
        return 1;
    }

    if (argc >= 3 && strcmp(argv[1], "--delete") == 0) {
        uint64_t delId = _strtoui64(argv[2], nullptr, 10);
        if (delId == 3823543010ULL) {
            printf("[ERROR] Refusing to delete the main RepublicMP Workshop item 3823543010!\n");
            return 1;
        }
        printf("[UGC] Deleting duplicate Workshop item %llu...\n", (unsigned long long)delId);
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

    SyncLatestModBinaries(repoRoot, wsDir, contentDir);

    // Always default to the existing RepublicMP Workshop item 3823543010 — NEVER create a duplicate item!
    uint64_t publishedId = 3823543010ULL;
    std::string existingIdStr = ReadFileUtf8(idFile.c_str());
    if (!existingIdStr.empty()) {
        uint64_t parsedId = _strtoui64(existingIdStr.c_str(), nullptr, 10);
        if (parsedId > 0) publishedId = parsedId;
    }
    if (argc > 1 && argv[1][0] != '-') {
        uint64_t argId = _strtoui64(argv[1], nullptr, 10);
        if (argId > 0) publishedId = argId;
    }

    printf("[UGC] Updating existing Steam Workshop item: %llu\n", (unsigned long long)publishedId);

    std::string desc = ReadFileUtf8(descFile.c_str());
    if (desc.empty()) {
        desc = "Co-op multiplayer mod for Workers & Resources: Soviet Republic (Lockstep, Steam P2P, One State & Two States modes, 30x30 km maps, Auto-Updater).";
    }

        bool updateMeta = (argc > 1 && (strcmp(argv[1], "--meta") == 0 || strcmp(argv[1], "--preview") == 0));
    bool updatePreview = (argc > 1 && strcmp(argv[1], "--preview") == 0);
    uint64_t hUpdate = pStartUpdate(pUGC, 784150, publishedId);
    bool bTitle = updateMeta ? pSetTitle(pUGC, hUpdate, "RepublicMP v" REPUBLICMP_VERSION " - Co-op Multiplayer (One State & Two States)") : true;
    bool bDesc = updateMeta ? pSetDesc(pUGC, hUpdate, desc.c_str()) : true;
    bool bVis = updateMeta ? pSetVis(pUGC, hUpdate, 1 /* k_ERemoteStoragePublishedFileVisibilityFriendsOnly */) : true;
    bool bContent = pSetContent(pUGC, hUpdate, contentDir.c_str());
    bool bPreview = updatePreview ? pSetPreview(pUGC, hUpdate, previewFile.c_str()) : true;
    printf("[UGC] SetItem: title=%d desc=%d vis=%d content=%d preview=%d (uploadPreview=%d)\n",
           (int)bTitle, (int)bDesc, (int)bVis, (int)bContent, (int)bPreview, (int)updatePreview);

    char defaultNote[512];
    _snprintf_s(defaultNote, sizeof(defaultNote), _TRUNCATE,
                "v%s (Protocol %d) - 18 co-op fixes (R1-R18): independent Two-States research trees, road/cable drag continuity, vehicle-owner customs trade, per-site financing, DLC road filter, civic/health split, C3D material & teardown crash guards",
                REPUBLICMP_VERSION, (int)REPUBLICMP_PROTOCOL);

    printf("[UGC] Submitting content (%s)...\n", contentDir.c_str());
    g_submitDone = false;
    uint64_t hSubmitCall = pSubmitUpdate(pUGC, hUpdate, defaultNote);
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

    DeleteFileA("steam_appid.txt");
    if (pShutdown) pShutdown();

    if (g_submitIOFail || g_submitResult.m_eResult != 1) {
        printf("[ERROR] SubmitItemUpdate failed! EResult=%d, IOFail=%d\n",
               (int)g_submitResult.m_eResult, (int)g_submitIOFail);
        return 1;
    }

    printf("[SUCCESS] Published to Steam Workshop!\n");
    printf("Workshop URL: https://steamcommunity.com/sharedfiles/filedetails/?id=%llu\n",
           (unsigned long long)publishedId);
    return 0;
}
