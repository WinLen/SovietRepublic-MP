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
typedef bool (*Fn_ISteamUGC_SetItemVisibility)(void* self, uint64_t handle, int32_t eVisibility);
typedef bool (*Fn_ISteamUGC_SetItemContent)(void* self, uint64_t handle, const char* pszContentFolder);
typedef bool (*Fn_ISteamUGC_SetItemPreview)(void* self, uint64_t handle, const char* pszPreviewFile);
typedef uint64_t (*Fn_ISteamUGC_SubmitItemUpdate)(void* self, uint64_t handle, const char* pchChangeNote);
typedef int32_t (*Fn_ISteamUGC_GetItemUpdateProgress)(void* self, uint64_t handle, uint64_t* punBytesProcessed, uint64_t* punBytesTotal);

typedef void (*CreateItemCallbackFn)(CreateItemResult_t* pParam, bool bIOFailure);
typedef void (*SubmitItemCallbackFn)(SubmitItemUpdateResult_t* pParam, bool bIOFailure);
typedef void* (*Fn_CCreateItemResult_t_SetCallResult)(uint64_t hAPICall, CreateItemCallbackFn fn);
typedef void* (*Fn_CSubmitItemUpdateResult_t_SetCallResult)(uint64_t hAPICall, SubmitItemCallbackFn fn);

static volatile bool g_createDone = false;
static CreateItemResult_t g_createResult = {};
static volatile bool g_createIOFail = false;

static volatile bool g_submitDone = false;
static SubmitItemUpdateResult_t g_submitResult = {};
static volatile bool g_submitIOFail = false;

static void OnCreateItem(CreateItemResult_t* pParam, bool bIOFailure)
{
    if (pParam) g_createResult = *pParam;
    g_createIOFail = bIOFailure;
    g_createDone = true;
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
    auto pSetVis = (Fn_ISteamUGC_SetItemVisibility)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemVisibility");
    auto pSetContent = (Fn_ISteamUGC_SetItemContent)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemContent");
    auto pSetPreview = (Fn_ISteamUGC_SetItemPreview)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SetItemPreview");
    auto pSubmitUpdate = (Fn_ISteamUGC_SubmitItemUpdate)GetProcAddress(hDll, "SteamAPI_ISteamUGC_SubmitItemUpdate");
    auto pGetProgress = (Fn_ISteamUGC_GetItemUpdateProgress)GetProcAddress(hDll, "SteamAPI_ISteamUGC_GetItemUpdateProgress");

    auto pSetCreateCall = (Fn_CCreateItemResult_t_SetCallResult)GetProcAddress(hDll, "CCreateItemResult_t_SetCallResult");
    auto pSetSubmitCall = (Fn_CSubmitItemUpdateResult_t_SetCallResult)GetProcAddress(hDll, "CSubmitItemUpdateResult_t_SetCallResult");

    if (!pInit || !pInit()) {
        printf("[ERROR] SteamAPI_Init() failed! Make sure Steam desktop client is running and logged in.\n");
        return 1;
    }

    void* pUGC = pFindIface(pGetHSteamUser(), "STEAMUGC_INTERFACE_VERSION012");
    if (!pUGC) {
        printf("[ERROR] Could not acquire STEAMUGC_INTERFACE_VERSION012!\n");
        return 1;
    }

    char rootDir[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, rootDir);
    std::string wsDir = std::string(rootDir) + "\\steam_workshop";
    std::string idFile = wsDir + "\\published_file_id.txt";
    std::string contentDir = wsDir + "\\content";
    std::string previewFile = wsDir + "\\preview.png";
    std::string descFile = wsDir + "\\description_ru.txt";

    uint64_t publishedId = 0;
    std::string existingIdStr = ReadFileUtf8(idFile.c_str());
    if (!existingIdStr.empty()) {
        publishedId = _strtoui64(existingIdStr.c_str(), nullptr, 10);
    }
    if (argc > 1 && argv[1][0]) {
        uint64_t argId = _strtoui64(argv[1], nullptr, 10);
        if (argId > 0) publishedId = argId;
    }

    if (publishedId == 0) {
        printf("[UGC] Creating new Steam Workshop item for AppID 784150...\n");
        g_createDone = false;
        uint64_t hCall = pCreateItem(pUGC, 784150, 0 /* k_EWorkshopFileTypeCommunity */);
        pSetCreateCall(hCall, OnCreateItem);
        while (!g_createDone) {
            pRunCallbacks();
            Sleep(50);
        }
        if (g_createIOFail || g_createResult.m_eResult != 1) {
            printf("[ERROR] CreateItem failed! EResult=%d, IOFail=%d\n",
                   (int)g_createResult.m_eResult, (int)g_createIOFail);
            return 1;
        }
        publishedId = g_createResult.m_nPublishedFileId;
        printf("[UGC] Created Workshop item! PublishedFileId = %llu\n", (unsigned long long)publishedId);
        if (g_createResult.m_bUserNeedsToAcceptWorkshopLegalAgreement) {
            printf("[NOTE] You may need to accept the Steam Workshop Legal Agreement on the item page.\n");
        }
        FILE* fid = nullptr;
        if (fopen_s(&fid, idFile.c_str(), "wb") == 0 && fid) {
            fprintf(fid, "%llu\n", (unsigned long long)publishedId);
            fclose(fid);
        }
    } else {
        printf("[UGC] Updating existing Steam Workshop item: %llu\n", (unsigned long long)publishedId);
    }

    std::string desc = ReadFileUtf8(descFile.c_str());
    if (desc.empty()) {
        desc = "Co-op multiplayer mod for Workers & Resources: Soviet Republic (Lockstep, Steam P2P, One State & Two States modes, 30x30 km maps, Auto-Updater).";
    }

    uint64_t hUpdate = pStartUpdate(pUGC, 784150, publishedId);
    bool bTitle = pSetTitle(pUGC, hUpdate, "RepublicMP — Кооператив / Co-op Multiplayer (One State & Two States)");
    bool bDesc = pSetDesc(pUGC, hUpdate, desc.c_str());
    bool bVis = pSetVis(pUGC, hUpdate, 1 /* k_ERemoteStoragePublishedFileVisibilityFriendsOnly */);
    bool bContent = pSetContent(pUGC, hUpdate, contentDir.c_str());
    bool bPreview = pSetPreview(pUGC, hUpdate, previewFile.c_str());
    printf("[UGC] SetItem: title=%d desc=%d vis=%d content=%d preview=%d\n",
           (int)bTitle, (int)bDesc, (int)bVis, (int)bContent, (int)bPreview);

    printf("[UGC] Submitting content (%s) and preview (%s)...\n", contentDir.c_str(), previewFile.c_str());
    g_submitDone = false;
    uint64_t hSubmitCall = pSubmitUpdate(pUGC, hUpdate, "v1.0.0 - Fix client road building crash/desync in Lockstep (RoadSync_ClearCategoryPreview + compact LS_CMD_SEGMENT + 4FCAF0 ledger)");
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
