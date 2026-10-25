// SPDX-License-Identifier: GPL-3.0-only
// See mod_sync.h.
#include "mod_sync.h"
#include "two_states.h"
#include "../save/save_zip.h"
#include "../ui/sync_block.h"
#include "../ui/ui_log.h"
#include "../ui/lang.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <system_error>

static const BYTE kRefreshPrologue[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, // mov [rsp+10h], rbx
    0x48, 0x89, 0x74, 0x24, 0x18, // mov [rsp+18h], rsi
    0x48, 0x89, 0x7C, 0x24, 0x20, // mov [rsp+20h], rdi
    0x55                          // push rbp
};

typedef void (__fastcall* Fn_WorkshopRefresh)(void* thisPtr, bool a2, bool skipWip, bool a4);
static Fn_WorkshopRefresh o_workshopRefresh = nullptr;

// C3DDLL64.dll Workshop forwarding exports
typedef void (__cdecl* Fn_WorkshopForwardAddItem)(uint64_t itemId, const char* folderPath);
static Fn_WorkshopForwardAddItem pWorkshopForwardAddItem = nullptr;

// SteamUGC dynamic bindings (steam_api64.dll)
typedef int32_t HSteamUser;
typedef HSteamUser (*Fn_SteamAPI_GetHSteamUser)();
typedef void* (*Fn_SteamInternal_FindOrCreateUserInterface)(HSteamUser hSteamUser, const char* pszVersion);
typedef uint32_t (*Fn_ISteamUGC_GetNumSubscribedItems)(void* self);
typedef uint32_t (*Fn_ISteamUGC_GetSubscribedItems)(void* self, uint64_t* pvecPublishedFileID, uint32_t cMaxEntries);
typedef uint32_t (*Fn_ISteamUGC_GetItemState)(void* self, uint64_t nPublishedFileID);
typedef bool (*Fn_ISteamUGC_GetItemInstallInfo)(void* self, uint64_t nPublishedFileID, uint64_t* punSizeOnDisk,
                                                char* pchFolder, uint32_t cchFolderSize, uint32_t* punTimeStamp);
typedef bool (*Fn_ISteamUGC_GetItemDownloadInfo)(void* self, uint64_t nPublishedFileID,
                                                 uint64_t* punBytesDownloaded, uint64_t* punBytesTotal);
typedef uint64_t (*Fn_ISteamUGC_SubscribeItem)(void* self, uint64_t nPublishedFileID);
typedef bool (*Fn_ISteamUGC_DownloadItem)(void* self, uint64_t nPublishedFileID, bool bHighPriority);

enum EItemStateBits {
    k_EItemStateNone        = 0,
    k_EItemStateSubscribed  = 1,
    k_EItemStateLegacyItem  = 2,
    k_EItemStateInstalled   = 4,
    k_EItemStateNeedsUpdate = 8,
    k_EItemStateDownloading = 16,
    k_EItemStateDownloadPending = 32,
};

static void* s_pSteamUGC = nullptr;
static Fn_ISteamUGC_GetNumSubscribedItems pUGC_GetNumSubscribedItems = nullptr;
static Fn_ISteamUGC_GetSubscribedItems    pUGC_GetSubscribedItems    = nullptr;
static Fn_ISteamUGC_GetItemState          pUGC_GetItemState          = nullptr;
static Fn_ISteamUGC_GetItemInstallInfo    pUGC_GetItemInstallInfo    = nullptr;
static Fn_ISteamUGC_GetItemDownloadInfo   pUGC_GetItemDownloadInfo   = nullptr;
static Fn_ISteamUGC_SubscribeItem         pUGC_SubscribeItem         = nullptr;
static Fn_ISteamUGC_DownloadItem          pUGC_DownloadItem          = nullptr;

// TesmioLoader plugin manager service
static const TsmPluginManagerApi* g_pluginMgr = nullptr;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static INIT_ONCE g_modLockOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_modLock;

struct LocalModEntry {
    uint64_t id;
    bool     isWip;
    int      itemType;
    char     title[128];
    char     folderPath[260];
};

struct InternalPluginEntry {
    char id[64];
    char iniPath[260];
    bool localEnabled;
};

static ModPackMode g_packMode = MOD_PACK_ALL_SHARED;
static std::vector<LocalModEntry> g_mods;
static std::vector<uint64_t> g_lastSaveUsedIds;
static std::vector<InternalPluginEntry> g_plugins;

// Active Co-op session filter state
static volatile bool g_sessionFilterActive = false;
static volatile bool g_needMainThreadWorkshopRefresh = false;
static int g_hiddenTailCount = 0;
static std::vector<uint64_t> g_sessionAllowedIds;
static std::vector<std::pair<uint64_t, std::string>> g_sessionSandboxForwards;

#define TEMP_MOD_MARKER_FILE ".republicmp_session_temp"

static void LockMods()
{
    TsmEnsureCriticalSection(&g_modLockOnce, &g_modLock);
    EnterCriticalSection(&g_modLock);
}

static void UnlockMods()
{
    LeaveCriticalSection(&g_modLock);
}

static const TsmHost* g_host = nullptr;

void ModSync_Bind(const TsmHost* host)
{
    g_host = host;
    TsmBind(host);
}

static bool GetExeRootDir(char* outDir, size_t outSize)
{
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_exe, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) return false;
    *lastSlash = '\0';
    strncpy_s(outDir, outSize, exePath, _TRUNCATE);
    return true;
}

static bool InitSteamUGC()
{
    if (s_pSteamUGC) return true;
    HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
    if (!hSteam) return false;

    Fn_SteamAPI_GetHSteamUser fnGetUser =
        (Fn_SteamAPI_GetHSteamUser)GetProcAddress(hSteam, "SteamAPI_GetHSteamUser");
    Fn_SteamInternal_FindOrCreateUserInterface fnFindIface =
        (Fn_SteamInternal_FindOrCreateUserInterface)GetProcAddress(hSteam, "SteamInternal_FindOrCreateUserInterface");

    pUGC_GetNumSubscribedItems = (Fn_ISteamUGC_GetNumSubscribedItems)GetProcAddress(hSteam, "SteamAPI_ISteamUGC_GetNumSubscribedItems");
    pUGC_GetSubscribedItems    = (Fn_ISteamUGC_GetSubscribedItems)GetProcAddress(hSteam, "SteamAPI_ISteamUGC_GetSubscribedItems");
    pUGC_GetItemState          = (Fn_ISteamUGC_GetItemState)GetProcAddress(hSteam, "SteamAPI_ISteamUGC_GetItemState");
    pUGC_GetItemInstallInfo    = (Fn_ISteamUGC_GetItemInstallInfo)GetProcAddress(hSteam, "SteamAPI_ISteamUGC_GetItemInstallInfo");
    pUGC_GetItemDownloadInfo   = (Fn_ISteamUGC_GetItemDownloadInfo)GetProcAddress(hSteam, "SteamAPI_ISteamUGC_GetItemDownloadInfo");
    pUGC_SubscribeItem         = (Fn_ISteamUGC_SubscribeItem)GetProcAddress(hSteam, "SteamAPI_ISteamUGC_SubscribeItem");
    pUGC_DownloadItem          = (Fn_ISteamUGC_DownloadItem)GetProcAddress(hSteam, "SteamAPI_ISteamUGC_DownloadItem");

    if (!fnGetUser || !fnFindIface || !pUGC_GetItemState || !pUGC_GetItemInstallInfo) return false;

    HSteamUser hUser = fnGetUser();
    if (!hUser) return false; // SteamAPI_Init() has not run yet

    // SOVIET64.exe + steam_api64.dll flat thunks are compiled against STEAMUGC_INTERFACE_VERSION012
    s_pSteamUGC = fnFindIface(hUser, "STEAMUGC_INTERFACE_VERSION012");
    if (s_pSteamUGC) {
        DiagLog_Printf("MOD_SYNC", "Bound ISteamUGC interface (STEAMUGC_INTERFACE_VERSION012, hUser=%d) at %p",
                       (int)hUser, s_pSteamUGC);
    }
    return s_pSteamUGC != nullptr;
}

static void CleanupTempSessionMods()
{
    char rootDir[MAX_PATH] = {};
    if (!GetExeRootDir(rootDir, sizeof(rootDir))) return;

    std::error_code ec;
    std::filesystem::path wipDir = std::filesystem::path(rootDir) / "media_soviet" / "workshop_wip";
    if (std::filesystem::exists(wipDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(wipDir, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::filesystem::path marker = entry.path() / TEMP_MOD_MARKER_FILE;
            if (std::filesystem::exists(marker, ec)) {
                std::string folderName = entry.path().filename().string();
                std::filesystem::remove_all(entry.path(), ec);
                Logf("republicmp  MOD_SYNC: removed temporary session mod folder %s", folderName.c_str());
            }
        }
    }

    std::filesystem::path sessionIniDir = std::filesystem::path(rootDir) / "plugins" / ".mp_session_ini";
    if (std::filesystem::exists(sessionIniDir, ec)) {
        std::filesystem::remove_all(sessionIniDir, ec);
    }
}

// ---------------------------------------------------------------------------
// POD SEH Helpers for SOVIET_WORKSHOP_MANAGER (No C++ destructors in __try!)
// ---------------------------------------------------------------------------

static bool IsRepublicMpSelfId(uint64_t id)
{
    return id == 3823543010ULL || id == 3822972914ULL;
}

static void* SafeGetEngineWorkshopMgr()
{
    void* ptr = nullptr;
    DiagLog_BeginSafeSeh();
    __try {
        if (g_exeBase) {
            ptr = *(void**)(g_exeBase + WORKSHOP_MGR_PTR_RVA);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ptr = nullptr;
    }
    DiagLog_EndSafeSeh();
    return ptr;
}

static int SafeGetWorkshopMgrCount(void* mgrPtr)
{
    int result = 0;
    DiagLog_BeginSafeSeh();
    __try {
        if (mgrPtr) {
            BYTE** begin = *(BYTE***)((BYTE*)mgrPtr + 0x00);
            BYTE** end   = *(BYTE***)((BYTE*)mgrPtr + 0x08);
            if (begin && end >= begin) {
                ptrdiff_t n = end - begin;
                if (n >= 0 && n <= 16384) result = (int)n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
    }
    DiagLog_EndSafeSeh();
    return result;
}

static bool SafeReadWorkshopMgrItem(void* mgrPtr, int index, uint64_t* idOut,
                                    bool* isWipOut, int* typeOut, char* titleOut, size_t titleMax)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** begin = *(BYTE***)((BYTE*)mgrPtr + 0x00);
        BYTE** end   = *(BYTE***)((BYTE*)mgrPtr + 0x08);
        if (begin && begin + index < end) {
            BYTE* item = begin[index];
            if (item) {
                WORD subWord = *(WORD*)(item + ITEM_IS_SUBSCRIBED_OFF);
                *isWipOut = (subWord == 0);
                *typeOut  = *(int*)(item + ITEM_TYPE_OFF);

                const char* idStr = (const char*)(item + ITEM_ID_STR_OFF);
                uint64_t id = _strtoui64(idStr, nullptr, 10);
                if (id == 0) id = *(uint64_t*)(item + ITEM_ID_QWORD_OFF);
                *idOut = id;

                const char* nameStr = (const char*)(item + ITEM_NAME_OFF);
                strncpy_s(titleOut, titleMax, (nameStr && nameStr[0]) ? nameStr : idStr, _TRUNCATE);
                ok = (id != 0);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static uint64_t SafeGetRawItemId(BYTE* item)
{
    uint64_t id = 0;
    DiagLog_BeginSafeSeh();
    __try {
        if (item) {
            const char* idStr = (const char*)(item + ITEM_ID_STR_OFF);
            id = _strtoui64(idStr, nullptr, 10);
            if (id == 0) id = *(uint64_t*)(item + ITEM_ID_QWORD_OFF);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id = 0;
    }
    DiagLog_EndSafeSeh();
    return id;
}

static int SafeGetRawItemType(BYTE* item)
{
    int t = -1;
    DiagLog_BeginSafeSeh();
    __try {
        if (item) t = *(int*)(item + ITEM_TYPE_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t = -1;
    }
    DiagLog_EndSafeSeh();
    return t;
}

// Restores any excluded SOVIET_WORKSHOP_ITEM* pointers back into the active vector range
// [begin .. _Mylast) so singleplayer mods are immediately available and o_workshopRefresh
// frees 100% of allocated items without leaking.
static void SafeRestoreWorkshopMgr(void* mgrPtr, int* ioHiddenTail)
{
    if (!mgrPtr || !ioHiddenTail || *ioHiddenTail <= 0) return;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** begin = *(BYTE***)((BYTE*)mgrPtr + 0x00);
        BYTE** end   = *(BYTE***)((BYTE*)mgrPtr + 0x08);
        BYTE** cap   = *(BYTE***)((BYTE*)mgrPtr + 0x10);
        if (begin && end >= begin && end + *ioHiddenTail <= cap) {
            *(BYTE***)((BYTE*)mgrPtr + 0x08) = end + *ioHiddenTail;
        }
        *ioHiddenTail = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioHiddenTail = 0;
    }
    DiagLog_EndSafeSeh();
}

// Partitions and sorts the SOVIET_WORKSHOP_MANAGER vector in-place so only
// allowedIds (plus pure text localization items) remain in [begin .. begin + kept)
// in ascending ID order, while excluded items are preserved in [begin + kept .. begin + total).
static int SafeFilterAndSortWorkshopMgr(void* mgrPtr, const uint64_t* allowedIds, int allowedCount, int* ioHiddenTail)
{
    if (!mgrPtr) return 0;
    SafeRestoreWorkshopMgr(mgrPtr, ioHiddenTail);

    int result = 0;
    DiagLog_BeginSafeSeh();
    __try {
        BYTE** begin = *(BYTE***)((BYTE*)mgrPtr + 0x00);
        BYTE** end   = *(BYTE***)((BYTE*)mgrPtr + 0x08);
        if (begin && end > begin) {
            int total = (int)(end - begin);
            if (total > 0 && total <= 16384) {
                int kept = 0;
                for (int i = 0; i < total; ++i) {
                    BYTE* item = begin[i];
                    if (!item) continue;
                    uint64_t id = SafeGetRawItemId(item);
                    int itemType = SafeGetRawItemType(item);

                    bool keep = (itemType == 4); // WORKSHOP_ITEMTYPE_TEXT (localization)
                    if (!keep && allowedIds) {
                        for (int k = 0; k < allowedCount; ++k) {
                            if (allowedIds[k] == id) {
                                keep = true;
                                break;
                            }
                        }
                    }
                    if (keep) {
                        if (kept != i) {
                            BYTE* tmp = begin[kept];
                            begin[kept] = begin[i];
                            begin[i] = tmp;
                        }
                        ++kept;
                    }
                }

                // Insertion sort [0 .. kept - 1] by Workshop ID so Host and Client register
                // building and vehicle types in the exact same deterministic order.
                for (int i = 1; i < kept; ++i) {
                    BYTE* keyItem = begin[i];
                    uint64_t keyId = SafeGetRawItemId(keyItem);
                    int j = i - 1;
                    while (j >= 0 && SafeGetRawItemId(begin[j]) > keyId) {
                        begin[j + 1] = begin[j];
                        --j;
                    }
                    begin[j + 1] = keyItem;
                }

                *(BYTE***)((BYTE*)mgrPtr + 0x08) = begin + kept;
                if (ioHiddenTail) *ioHiddenTail = total - kept;
                result = kept;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    DiagLog_EndSafeSeh();
    return result;
}

// ---------------------------------------------------------------------------
// Parsing workshopconfig.ini & Save header.bin
// ---------------------------------------------------------------------------

static void ReadWorkshopConfigBasic(const char* configPath, char* titleOut, size_t titleMax, int* typeOut)
{
    FILE* f = nullptr;
    if (fopen_s(&f, configPath, "rb") != 0 || !f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "$ITEM_TYPE")) {
            if (strstr(line, "WORKSHOP_ITEMTYPE_VEHICLE") && !strstr(line, "SKIN")) *typeOut = 0;
            else if (strstr(line, "WORKSHOP_ITEMTYPE_BUILDING") && !strstr(line, "SKIN") && !strstr(line, "ELEMENT")) *typeOut = 1;
            else if (strstr(line, "WORKSHOP_ITEMTYPE_LANDSCAPE")) *typeOut = 2;
            else if (strstr(line, "WORKSHOP_ITEMTYPE_BUILDINGSKIN")) *typeOut = 3;
            else if (strstr(line, "WORKSHOP_ITEMTYPE_TEXT")) *typeOut = 4;
            else if (strstr(line, "WORKSHOP_ITEMTYPE_VEHICLESKIN")) *typeOut = 5;
            else if (strstr(line, "WORKSHOP_ITEMTYPE_BUILDINGEDITOR_ELEMENT")) *typeOut = 6;
            else if (strstr(line, "WORKSHOP_ITEMTYPE_SCRIPT")) *typeOut = 7;
        } else if (strstr(line, "$ITEM_NAME")) {
            const char* q1 = strchr(line, '"');
            if (q1) {
                ++q1;
                const char* q2 = strrchr(q1, '"');
                size_t len = q2 ? (size_t)(q2 - q1) : strlen(q1);
                if (len > 0 && len < titleMax) {
                    strncpy_s(titleOut, titleMax, q1, len);
                }
            }
        }
    }
    fclose(f);
}

static bool IsValidWorkshopModId(uint64_t id)
{
    if (id == 0 || IsRepublicMpSelfId(id)) return false;
    // Steam Workshop PublishedFileIDs for AppID 784150 are 9-11 digits (100,000,000 .. 99,999,999,999)
    if (id >= 100000000ULL && id <= 99999999999ULL) return true;
    // Or a local workshop_wip folder exists on disk for smaller custom IDs
    char rootDir[MAX_PATH] = {};
    if (GetExeRootDir(rootDir, sizeof(rootDir))) {
        char wipCfg[MAX_PATH];
        _snprintf_s(wipCfg, sizeof(wipCfg), _TRUNCATE,
                    "%s\\media_soviet\\workshop_wip\\%llu\\workshopconfig.ini",
                    rootDir, (unsigned long long)id);
        if (GetFileAttributesA(wipCfg) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

void ModSync_ExtractHeaderBinModIdsFromBuffer(const void* data, size_t size, uint64_t* outIds, int maxOut, int* ioCount)
{
    if (!data || !outIds || maxOut <= 0 || !ioCount) return;
    if (size < HEADER_BIN_DATA_OFFSET + HEADER_BIN_RECORD_SIZE) return;

    const BYTE* base = (const BYTE*)data + HEADER_BIN_DATA_OFFSET;
    size_t numRecords = (size - HEADER_BIN_DATA_OFFSET) / HEADER_BIN_RECORD_SIZE;
    if (numRecords > 100000) numRecords = 100000;

    for (size_t i = 0; i < numRecords; ++i) {
        const char* rec = (const char*)(base + i * HEADER_BIN_RECORD_SIZE);
        char nameBuf[104] = {};
        memcpy(nameBuf, rec, 100);
        nameBuf[100] = '\0';

        const char* p = nameBuf;
        if (_strnicmp(p, "MIRRORZ_", 8) == 0) {
            p += 8;
        }
        const char* slash = strchr(p, '/');
        if (!slash) slash = strchr(p, '\\');
        if (slash && slash > p && (slash - p) <= 20) {
            bool allDigits = true;
            for (const char* c = p; c < slash; ++c) {
                if (*c < '0' || *c > '9') {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) {
                uint64_t wsId = _strtoui64(p, nullptr, 10);
                if (!IsValidWorkshopModId(wsId)) continue;
                bool exists = false;
                for (int k = 0; k < *ioCount; ++k) {
                    if (outIds[k] == wsId) { exists = true; break; }
                }
                if (!exists && *ioCount < maxOut) {
                    outIds[(*ioCount)++] = wsId;
                }
            }
        }
    }
}

int ModSync_FilterAndSortWorkshopMgrForTest(void* mgrPtr, const uint64_t* allowedIds, int allowedCount, int* ioHiddenTail)
{
    return SafeFilterAndSortWorkshopMgr(mgrPtr, allowedIds, allowedCount, ioHiddenTail);
}

void ModSync_RestoreWorkshopMgrForTest(void* mgrPtr, int* ioHiddenTail)
{
    SafeRestoreWorkshopMgr(mgrPtr, ioHiddenTail);
}

static void ExtractHeaderFileModIds(const char* headerPath, std::vector<uint64_t>& outIds)
{
    FILE* f = nullptr;
    if (fopen_s(&f, headerPath, "rb") != 0 || !f) return;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    // header.bin / headerE.bin have a 0x260-byte header followed by 0x80-byte records.
    if (fileSize < HEADER_BIN_DATA_OFFSET + HEADER_BIN_RECORD_SIZE) {
        fclose(f);
        return;
    }

    auto pushUnique = [&](uint64_t id) {
        if (!IsValidWorkshopModId(id)) return;
        if (std::find(outIds.begin(), outIds.end(), id) == outIds.end())
            outIds.push_back(id);
    };

    fseek(f, HEADER_BIN_DATA_OFFSET, SEEK_SET);
    long numRecords = (fileSize - HEADER_BIN_DATA_OFFSET) / HEADER_BIN_RECORD_SIZE;
    if (numRecords > 100000) numRecords = 100000;

    char rec[HEADER_BIN_RECORD_SIZE];
    for (long i = 0; i < numRecords; ++i) {
        if (fread(rec, 1, HEADER_BIN_RECORD_SIZE, f) != HEADER_BIN_RECORD_SIZE) break;
        uint64_t parsedId = 0;
        int parsedCount = 0;
        BYTE singleRecBuf[HEADER_BIN_DATA_OFFSET + HEADER_BIN_RECORD_SIZE] = {};
        memcpy(singleRecBuf + HEADER_BIN_DATA_OFFSET, rec, HEADER_BIN_RECORD_SIZE);
        ModSync_ExtractHeaderBinModIdsFromBuffer(singleRecBuf, sizeof(singleRecBuf), &parsedId, 1, &parsedCount);
        if (parsedCount == 1) {
            pushUnique(parsedId);
        }
    }

    fclose(f);
}

static void InspectSaveModsInternal(const char* saveDir, std::vector<uint64_t>& outIds)
{
    if (!saveDir || !saveDir[0]) return;
    size_t beforeCount = outIds.size();
    char p1[MAX_PATH], p2[MAX_PATH];
    _snprintf_s(p1, sizeof(p1), _TRUNCATE, "%s\\header.bin", saveDir);
    _snprintf_s(p2, sizeof(p2), _TRUNCATE, "%s\\headerE.bin", saveDir);
    ExtractHeaderFileModIds(p1, outIds);
    ExtractHeaderFileModIds(p2, outIds);

    size_t added = outIds.size() - beforeCount;
    if (added > 0) {
        std::string idList;
        for (size_t i = beforeCount; i < outIds.size(); ++i) {
            if (!idList.empty()) idList += ",";
            char num[32];
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%llu", (unsigned long long)outIds[i]);
            idList += num;
        }
        DiagLog_Printf("MOD_SYNC", "InspectSaveMods [%s]: found %zu Workshop mod(s): [%s]",
                       saveDir, added, idList.c_str());
    }
}

// ---------------------------------------------------------------------------
// Hook_WorkshopRefresh (RVA 0x8407C0)
// ---------------------------------------------------------------------------

static void CatalogFromEngineManager(void* mgrPtr)
{
    int count = SafeGetWorkshopMgrCount(mgrPtr);
    if (count <= 0) return;

    InitSteamUGC();
    char rootDir[MAX_PATH] = {};
    GetExeRootDir(rootDir, sizeof(rootDir));

    for (int i = 0; i < count; ++i) {
        uint64_t id = 0;
        bool isWip = false;
        int itemType = 1;
        char title[128] = {};
        if (!SafeReadWorkshopMgrItem(mgrPtr, i, &id, &isWip, &itemType, title, sizeof(title)))
            continue;
        if (!IsValidWorkshopModId(id))
            continue;

        // Skip temporary session mods downloaded into workshop_wip
        if (isWip && rootDir[0]) {
            char marker[MAX_PATH];
            _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\media_soviet\\workshop_wip\\%llu\\%s",
                        rootDir, (unsigned long long)id, TEMP_MOD_MARKER_FILE);
            if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES)
                continue;
        }

        bool exists = false;
        for (const auto& m : g_mods) {
            if (m.id == id) { exists = true; break; }
        }
        if (!exists) {
            LocalModEntry e = {};
            e.id = id;
            e.isWip = isWip;
            e.itemType = itemType;
            strncpy_s(e.title, sizeof(e.title), title, _TRUNCATE);
            if (isWip && rootDir[0]) {
                _snprintf_s(e.folderPath, sizeof(e.folderPath), _TRUNCATE,
                            "%s\\media_soviet\\workshop_wip\\%llu", rootDir, (unsigned long long)id);
            } else if (!isWip && s_pSteamUGC && pUGC_GetItemInstallInfo) {
                uint64_t sz = 0;
                uint32_t ts = 0;
                char folder[MAX_PATH] = {};
                if (pUGC_GetItemInstallInfo(s_pSteamUGC, id, &sz, folder, sizeof(folder), &ts)) {
                    strncpy_s(e.folderPath, sizeof(e.folderPath), folder, _TRUNCATE);
                }
            }
            g_mods.push_back(e);
        }
    }
}

static void __fastcall Hook_WorkshopRefresh(void* thisPtr, bool a2, bool skipWip, bool a4)
{
    LockMods();
    // Restore any hidden tail items BEFORE o_workshopRefresh runs so the engine
    // frees 100% of previously allocated SOVIET_WORKSHOP_ITEM* objects without leaking.
    SafeRestoreWorkshopMgr(thisPtr, &g_hiddenTailCount);
    if (g_sessionFilterActive) {
        // Ensure workshop_wip sandbox mods are always scanned during a Co-op session
        skipWip = false;
        if (pWorkshopForwardAddItem) {
            for (const auto& kv : g_sessionSandboxForwards) {
                pWorkshopForwardAddItem(kv.first, kv.second.c_str());
            }
        }
    }
    UnlockMods();

    o_workshopRefresh(thisPtr, a2, skipWip, a4);

    LockMods();
    g_needMainThreadWorkshopRefresh = false;
    CatalogFromEngineManager(thisPtr);

    if (g_sessionFilterActive) {
        int before = SafeGetWorkshopMgrCount(thisPtr);
        int kept = SafeFilterAndSortWorkshopMgr(
            thisPtr,
            g_sessionAllowedIds.empty() ? nullptr : g_sessionAllowedIds.data(),
            (int)g_sessionAllowedIds.size(),
            &g_hiddenTailCount);
        Logf("republicmp  MOD_SYNC: filtered SOVIET_WORKSHOP_MANAGER for Co-op session (%d -> %d items, hidden=%d, allowed=%zu)",
             before, kept, g_hiddenTailCount, g_sessionAllowedIds.size());
    }
    UnlockMods();
}

// ---------------------------------------------------------------------------
// Catalog & Settings
// ---------------------------------------------------------------------------

void ModSync_RefreshCatalog()
{
    LockMods();

    char rootDir[MAX_PATH] = {};
    GetExeRootDir(rootDir, sizeof(rootDir));

    // 1. Scan latest Co-op save for used Workshop IDs
    g_lastSaveUsedIds.clear();
    if (rootDir[0]) {
        char lastName[128] = {};
        if (TwoStates_GetLatestCoopSaveName(lastName, sizeof(lastName)) && lastName[0]) {
            char candidate[MAX_PATH];
            _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\media_soviet\\save_coop\\%s", rootDir, lastName);
            if (GetFileAttributesA(candidate) == INVALID_FILE_ATTRIBUTES) {
                _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\media_soviet\\save\\%s", rootDir, lastName);
            }
            if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
                InspectSaveModsInternal(candidate, g_lastSaveUsedIds);
            }
        }
    }

    // 2. Refresh Workshop & WIP catalog
    g_mods.clear();
    if (void* mgrPtr = SafeGetEngineWorkshopMgr()) {
        SafeRestoreWorkshopMgr(mgrPtr, &g_hiddenTailCount);
        CatalogFromEngineManager(mgrPtr);
        if (g_sessionFilterActive) {
            SafeFilterAndSortWorkshopMgr(
                mgrPtr,
                g_sessionAllowedIds.empty() ? nullptr : g_sessionAllowedIds.data(),
                (int)g_sessionAllowedIds.size(),
                &g_hiddenTailCount);
        }
    }

    // Also scan SteamUGC directly in case RefreshItems hasn't run yet
    if (InitSteamUGC() && pUGC_GetNumSubscribedItems && pUGC_GetSubscribedItems) {
        uint32_t numSub = pUGC_GetNumSubscribedItems(s_pSteamUGC);
        if (numSub > 0 && numSub < 8192) {
            std::vector<uint64_t> ids(numSub);
            uint32_t got = pUGC_GetSubscribedItems(s_pSteamUGC, ids.data(), numSub);
            for (uint32_t i = 0; i < got; ++i) {
                uint64_t id = ids[i];
                if (!IsValidWorkshopModId(id)) continue;
                bool found = false;
                for (const auto& m : g_mods) {
                    if (m.id == id) { found = true; break; }
                }
                if (found) continue;

                uint64_t sz = 0;
                uint32_t ts = 0;
                char folder[MAX_PATH] = {};
                if (pUGC_GetItemInstallInfo(s_pSteamUGC, id, &sz, folder, sizeof(folder), &ts) && folder[0]) {
                    char cfgPath[MAX_PATH];
                    _snprintf_s(cfgPath, sizeof(cfgPath), _TRUNCATE, "%s\\workshopconfig.ini", folder);
                    if (GetFileAttributesA(cfgPath) == INVALID_FILE_ATTRIBUTES) continue;
                    LocalModEntry e = {};
                    e.id = id;
                    e.isWip = false;
                    e.itemType = 1;
                    _snprintf_s(e.title, sizeof(e.title), _TRUNCATE, "Workshop #%llu", (unsigned long long)id);
                    strncpy_s(e.folderPath, sizeof(e.folderPath), folder, _TRUNCATE);
                    ReadWorkshopConfigBasic(cfgPath, e.title, sizeof(e.title), &e.itemType);
                    g_mods.push_back(e);
                }
            }
        }
    }

    // Also scan media_soviet\workshop_wip directly
    if (rootDir[0]) {
        std::error_code ec;
        std::filesystem::path wipRoot = std::filesystem::path(rootDir) / "media_soviet" / "workshop_wip";
        if (std::filesystem::exists(wipRoot, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(wipRoot, ec)) {
                if (!entry.is_directory(ec)) continue;
                if (std::filesystem::exists(entry.path() / TEMP_MOD_MARKER_FILE, ec)) continue;
                std::string folderName = entry.path().filename().string();
                uint64_t id = _strtoui64(folderName.c_str(), nullptr, 10);
                if (id == 0) continue;

                bool found = false;
                for (const auto& m : g_mods) {
                    if (m.id == id) { found = true; break; }
                }
                if (found) continue;

                LocalModEntry e = {};
                e.id = id;
                e.isWip = true;
                e.itemType = 1;
                _snprintf_s(e.title, sizeof(e.title), _TRUNCATE, "WIP #%llu", (unsigned long long)id);
                strncpy_s(e.folderPath, sizeof(e.folderPath), entry.path().string().c_str(), _TRUNCATE);
                std::string cfgPath = (entry.path() / "workshopconfig.ini").string();
                ReadWorkshopConfigBasic(cfgPath.c_str(), e.title, sizeof(e.title), &e.itemType);
                g_mods.push_back(e);
            }
        }
    }

    // 3. Refresh TesmioLoader plugins (for transparent ini sync if any are enabled in tesmioloader.ini)
    g_plugins.clear();
    if (!g_pluginMgr && g_host && g_host->consume) {
        g_pluginMgr = (const TsmPluginManagerApi*)g_host->consume(TSM_SERVICE_PLUGINS, TSM_PLUGINS_VERSION);
    }
    if (g_pluginMgr) {
        int n = g_pluginMgr->count();
        for (int i = 0; i < n; ++i) {
            InternalPluginEntry pe = {};
            int locEn = 0, effEn = 0;
            if (g_pluginMgr->getInfo(i, pe.id, sizeof(pe.id), pe.iniPath, sizeof(pe.iniPath), &locEn, &effEn)) {
                pe.localEnabled = (locEn != 0);
                g_plugins.push_back(pe);
            }
        }
    }

    DiagLog_Printf("MOD_SYNC", "Catalog refreshed: %zu Workshop/WIP mod(s), %zu used in latest save",
                   g_mods.size(), g_lastSaveUsedIds.size());
    UnlockMods();
}

void ModSync_Init()
{
    LockMods();
    CleanupTempSessionMods();
    UnlockMods();

    HMODULE hC3d = GetModuleHandleA("C3DDLL64.dll");
    if (hC3d) {
        pWorkshopForwardAddItem = (Fn_WorkshopForwardAddItem)GetProcAddress(
            hC3d, "?C3DPath_WorkshopForwardAddItem@@YAX_KPEAD@Z");
    }

    if (g_host && g_host->consume) {
        g_pluginMgr = (const TsmPluginManagerApi*)g_host->consume(TSM_SERVICE_PLUGINS, TSM_PLUGINS_VERSION);
    }

    if (g_exeBase && !o_workshopRefresh) {
        BYTE* target = g_exeBase + RVA_WORKSHOP_REFRESH;
        if (InstallInlineHook(target, (void*)&Hook_WorkshopRefresh, (void**)&o_workshopRefresh,
                              kRefreshPrologue, sizeof(kRefreshPrologue), "ModSync_WorkshopRefresh")) {
            Logf("republicmp  MOD_SYNC: hooked SOVIET_WORKSHOP_MANAGER::RefreshItems at RVA 0x%X",
                 RVA_WORKSHOP_REFRESH);
        }
    }
}

ModPackMode ModSync_GetPackMode()
{
    return g_packMode;
}

void ModSync_SetPackMode(ModPackMode mode)
{
    if (mode < MOD_PACK_SAVE_ONLY || mode > MOD_PACK_HOST_ONLY) mode = MOD_PACK_ALL_SHARED;
    g_packMode = mode;
    DiagLog_Printf("MOD_SYNC", "Server ModPackMode changed to %d", (int)g_packMode);
    TwoStates_SaveSettings();
}

void ModSync_CyclePackMode()
{
    // Cycle: MOD_PACK_ALL_SHARED (1) -> MOD_PACK_HOST_ONLY (2) -> MOD_PACK_SAVE_ONLY (0) -> MOD_PACK_ALL_SHARED (1)
    ModPackMode next = MOD_PACK_ALL_SHARED;
    if (g_packMode == MOD_PACK_ALL_SHARED) next = MOD_PACK_HOST_ONLY;
    else if (g_packMode == MOD_PACK_HOST_ONLY) next = MOD_PACK_SAVE_ONLY;
    else next = MOD_PACK_ALL_SHARED;
    ModSync_SetPackMode(next);
}

int ModSync_GetModCount()
{
    LockMods();
    int n = (int)g_mods.size();
    UnlockMods();
    return n;
}

int ModSync_GetActiveServerModCount()
{
    LockMods();
    int count = 0;
    if (g_sessionFilterActive) {
        count = (int)g_sessionAllowedIds.size();
    } else if (g_packMode == MOD_PACK_SAVE_ONLY) {
        count = (int)g_lastSaveUsedIds.size();
    } else {
        count = (int)g_mods.size();
    }
    UnlockMods();
    return count;
}

void ModSync_LoadSettingsFromIni(const char* iniPath)
{
    if (!iniPath || !iniPath[0]) return;
    LockMods();
    int mode = GetPrivateProfileIntA("mods", "auto_sync_mode", (int)MOD_PACK_ALL_SHARED, iniPath);
    if (mode < 0 || mode > 2) mode = (int)MOD_PACK_ALL_SHARED;
    g_packMode = (ModPackMode)mode;
    DiagLog_Printf("MOD_SYNC", "Loaded settings from [%s]: auto_sync_mode=%d", iniPath, (int)g_packMode);
    UnlockMods();
}

void ModSync_SaveSettingsToIni(const char* iniPath)
{
    if (!iniPath || !iniPath[0]) return;
    LockMods();
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", (int)g_packMode);
    WritePrivateProfileStringA("mods", "auto_sync_mode", buf, iniPath);
    UnlockMods();
}

// ---------------------------------------------------------------------------
// Wire Protocol for Bidirectional Auto-Sync Handshake (Host <-> Client)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct WireModManifestHeader {
    char     magic[8];       // "RPMPMOD2"
    uint32_t packMode;       // ModPackMode (0=SaveOnly, 1=AllShared, 2=HostOnly)
    uint32_t modCount;       // Number of WireModEntry records from Host
    uint32_t pluginCount;    // Number of WirePluginIniHeader records from Host
    char     enabledPluginsCsv[256];
};

struct WireModEntry {
    uint64_t id;
    uint8_t  isWip;
    uint8_t  itemType;
    char     title[64];
};

struct WireClientModListHeader {
    char     magic[8];       // "RPMPCMDS"
    uint32_t extraModCount;  // Number of additional WireModEntry records from Client
};

struct WirePluginIniHeader {
    char     id[64];
    uint32_t byteSize;
};

struct WireModFallbackReq {
    char     magic[8];       // "RPMPMREQ"
    uint32_t requestCount;   // Number of uint64_t IDs requested for direct transfer
};
#pragma pack(pop)

static bool FindLocalModFolder(uint64_t id, char* outFolder, size_t outSize, bool* isWipOut)
{
    char rootDir[MAX_PATH] = {};
    if (GetExeRootDir(rootDir, sizeof(rootDir))) {
        char wipCandidate[MAX_PATH];
        _snprintf_s(wipCandidate, sizeof(wipCandidate), _TRUNCATE,
                    "%s\\media_soviet\\workshop_wip\\%llu\\workshopconfig.ini",
                    rootDir, (unsigned long long)id);
        if (GetFileAttributesA(wipCandidate) != INVALID_FILE_ATTRIBUTES) {
            _snprintf_s(outFolder, outSize, _TRUNCATE,
                        "%s\\media_soviet\\workshop_wip\\%llu",
                        rootDir, (unsigned long long)id);
            if (isWipOut) *isWipOut = true;
            return true;
        }
    }

    if (InitSteamUGC() && pUGC_GetItemInstallInfo && pUGC_GetItemState) {
        uint32_t st = pUGC_GetItemState(s_pSteamUGC, id);
        if (st & k_EItemStateInstalled) {
            uint64_t sz = 0;
            uint32_t ts = 0;
            char folder[MAX_PATH] = {};
            if (pUGC_GetItemInstallInfo(s_pSteamUGC, id, &sz, folder, sizeof(folder), &ts) && folder[0]) {
                char cfg[MAX_PATH];
                _snprintf_s(cfg, sizeof(cfg), _TRUNCATE, "%s\\workshopconfig.ini", folder);
                if (GetFileAttributesA(cfg) != INVALID_FILE_ATTRIBUTES) {
                    strncpy_s(outFolder, outSize, folder, _TRUNCATE);
                    if (isWipOut) *isWipOut = false;
                    return true;
                }
            }
        }
    }
    return false;
}

static void BuildHostActiveModListLocked(const std::vector<uint64_t>& saveIds, std::vector<uint64_t>& outActiveIds)
{
    outActiveIds = saveIds;
    if (g_packMode != MOD_PACK_SAVE_ONLY) {
        for (const auto& m : g_mods) {
            if (m.itemType == 4) continue; // skip pure local text translation mods
            if (std::find(outActiveIds.begin(), outActiveIds.end(), m.id) == outActiveIds.end())
                outActiveIds.push_back(m.id);
        }
    }
    std::sort(outActiveIds.begin(), outActiveIds.end());
}

// Auto-subscribes & downloads any missing Workshop mods via ISteamUGC (used by BOTH Host and Client).
// Any mod that is local WIP or fails/times out in Steam is added to `outFallbackIds`.
static bool DownloadMissingSteamMods(SOCKET sockOrInvalid, const std::vector<WireModEntry>& candidates,
                                     const char* roleTag, std::vector<uint64_t>& outFallbackIds,
                                     bool* ioAnyNewOnDisk, char* errOut, size_t errSize)
{
    InitSteamUGC();
    std::vector<WireModEntry> needSteamDownload;

    for (const auto& wm : candidates) {
        char localFolder[MAX_PATH] = {};
        bool localWip = false;
        if (FindLocalModFolder(wm.id, localFolder, sizeof(localFolder), &localWip)) {
            continue; // Already installed on disk!
        }
        if (wm.isWip || !s_pSteamUGC || !pUGC_DownloadItem) {
            if (std::find(outFallbackIds.begin(), outFallbackIds.end(), wm.id) == outFallbackIds.end())
                outFallbackIds.push_back(wm.id);
        } else {
            needSteamDownload.push_back(wm);
        }
    }

    for (size_t i = 0; i < needSteamDownload.size(); ++i) {
        const auto& wm = needSteamDownload[i];
        if (!IsValidWorkshopModId(wm.id)) continue;

        if (pUGC_SubscribeItem) pUGC_SubscribeItem(s_pSteamUGC, wm.id);
        bool dlStarted = pUGC_DownloadItem ? pUGC_DownloadItem(s_pSteamUGC, wm.id, true) : false;

        if (sockOrInvalid != INVALID_SOCKET) {
            char stepText[160];
            _snprintf_s(stepText, sizeof(stepText), _TRUNCATE,
                        Tr("Steam Workshop (%zu/%zu): %s", "Steam Workshop (%zu/%zu): %s"),
                        i + 1, needSteamDownload.size(), wm.title);
            SyncBlock_SetStep(2, 7, stepText, 0);
        }

        ULONGLONG startWait = GetTickCount64();
        ULONGLONG maxWaitMs = (sockOrInvalid == INVALID_SOCKET) ? 8000ULL : 30000ULL;
        bool installed = false;
        while (GetTickCount64() - startWait < maxWaitMs) {
            if (sockOrInvalid != INVALID_SOCKET && NetworkPeerGone(sockOrInvalid)) {
                if (errOut && errSize > 0) {
                    _snprintf_s(errOut, errSize, _TRUNCATE, "connection closed while downloading Workshop mods");
                }
                return false;
            }
            char folder[MAX_PATH] = {};
            bool isWip = false;
            if (FindLocalModFolder(wm.id, folder, sizeof(folder), &isWip)) {
                installed = true;
                if (ioAnyNewOnDisk) *ioAnyNewOnDisk = true;
                break;
            }
            if (pUGC_GetItemState) {
                uint32_t st = pUGC_GetItemState(s_pSteamUGC, wm.id);
                ULONGLONG elapsed = GetTickCount64() - startWait;
                if (elapsed >= 2000ULL &&
                    !(st & (k_EItemStateDownloading | k_EItemStateDownloadPending | k_EItemStateInstalled)) &&
                    (!dlStarted || !(st & k_EItemStateNeedsUpdate))) {
                    break; // Item is removed/unavailable on Steam Workshop; do not hang waiting
                }
            }
            if (sockOrInvalid != INVALID_SOCKET && pUGC_GetItemDownloadInfo) {
                uint64_t done = 0, total = 0;
                if (pUGC_GetItemDownloadInfo(s_pSteamUGC, wm.id, &done, &total) && total > 0) {
                    int pct = (int)((done * 100) / total);
                    SyncBlock_SetPercent(pct);
                }
            }
            if (sockOrInvalid == INVALID_SOCKET) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            Sleep(100);
        }

        if (!installed) {
            Logf("republicmp  MOD_SYNC: [%s] Steam download for %llu (%s) timed out or unavailable - requesting direct transfer",
                 roleTag, (unsigned long long)wm.id, wm.title);
            if (std::find(outFallbackIds.begin(), outFallbackIds.end(), wm.id) == outFallbackIds.end())
                outFallbackIds.push_back(wm.id);
        } else {
            Logf("republicmp  MOD_SYNC: [%s] SteamUGC auto-installed mod %llu (%s)",
                 roleTag, (unsigned long long)wm.id, wm.title);
        }
    }
    return true;
}

static bool StreamRequestedModZips(SOCKET sock, const std::vector<uint64_t>& requestedIds,
                                   const char* roleTag, char* errOut, size_t errSize)
{
    char rootDir[MAX_PATH] = {};
    GetExeRootDir(rootDir, sizeof(rootDir));

    uint32_t reqCount = (uint32_t)requestedIds.size();
    for (uint32_t i = 0; i < reqCount; ++i) {
        uint64_t modId = requestedIds[i];
        char modFolder[MAX_PATH] = {};
        bool isWip = false;
        uint8_t hasZip = 0;

        char tempZip[MAX_PATH] = {};
        _snprintf_s(tempZip, sizeof(tempZip), _TRUNCATE, "%s\\media_soviet\\save_coop\\.mp_mod_send_%llu.zip",
                    rootDir, (unsigned long long)modId);

        if (FindLocalModFolder(modId, modFolder, sizeof(modFolder), &isWip)) {
            char statusText[128];
            _snprintf_s(statusText, sizeof(statusText), _TRUNCATE,
                        Tr("Передача мода #%llu партнёру (%u/%u)...", "Sending mod #%llu to partner (%u/%u)..."),
                        (unsigned long long)modId, i + 1, reqCount);
            SyncBlock_SetStep(2, 7, statusText, (int)(((i + 1) * 100) / reqCount));

            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(rootDir) / "media_soviet" / "save_coop", ec);
            if (ZipSaveDirectory(modFolder, tempZip, nullptr, nullptr)) {
                hasZip = 1;
            }
        } else {
            DiagLog_Printf("MOD_SYNC", "[%s] WARNING: requested fallback mod %llu not found locally on disk!",
                           roleTag, (unsigned long long)modId);
        }

        if (!NetworkSendExact(sock, &hasZip, sizeof(hasZip))) {
            DeleteFileA(tempZip);
            _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection sending fallback mod header");
            return false;
        }
        if (hasZip) {
            bool sent = NetworkSendFile(sock, tempZip, nullptr, nullptr);
            DeleteFileA(tempZip);
            if (!sent) {
                _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection streaming fallback mod #%llu",
                            (unsigned long long)modId);
                return false;
            }
            Logf("republicmp  MOD_SYNC: [%s] streamed mod %llu (%s) to partner sandbox",
                 roleTag, (unsigned long long)modId, isWip ? "WIP" : "Workshop fallback");
        } else {
            DeleteFileA(tempZip);
        }
    }
    return true;
}

static bool ReceiveRequestedModZips(SOCKET sock, const std::vector<uint64_t>& requestedIds,
                                    const char* roleTag, bool* ioAnyNewOnDisk, char* errOut, size_t errSize)
{
    char rootDir[MAX_PATH] = {};
    GetExeRootDir(rootDir, sizeof(rootDir));
    std::error_code ec;

    uint32_t reqCount = (uint32_t)requestedIds.size();
    for (uint32_t i = 0; i < reqCount; ++i) {
        uint64_t modId = requestedIds[i];
        uint8_t hasZip = 0;
        if (!NetworkRecvExact(sock, &hasZip, sizeof(hasZip))) {
            _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving fallback mod header");
            return false;
        }
        if (!hasZip) continue;

        char statusText[128];
        _snprintf_s(statusText, sizeof(statusText), _TRUNCATE,
                    Tr("Приём мода #%llu от партнёра (%u/%u)...", "Receiving mod #%llu from partner (%u/%u)..."),
                    (unsigned long long)modId, i + 1, reqCount);
        SyncBlock_SetStep(2, 7, statusText, (int)(((i + 1) * 100) / reqCount));

        char tempZip[MAX_PATH] = {};
        _snprintf_s(tempZip, sizeof(tempZip), _TRUNCATE,
                    "%s\\media_soviet\\save_coop\\.mp_mod_recv_%llu.zip",
                    rootDir, (unsigned long long)modId);
        std::filesystem::create_directories(std::filesystem::path(rootDir) / "media_soviet" / "save_coop", ec);

        if (!NetworkReceiveFile(sock, tempZip, nullptr, nullptr)) {
            DeleteFileA(tempZip);
            _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving mod #%llu",
                        (unsigned long long)modId);
            return false;
        }

        std::filesystem::path targetDir = std::filesystem::path(rootDir) / "media_soviet" / "workshop_wip" / std::to_string(modId);
        bool existedBefore = std::filesystem::exists(targetDir, ec);
        std::filesystem::create_directories(targetDir, ec);

        UnzipToDirectory(tempZip, targetDir.string().c_str(), nullptr, nullptr);
        DeleteFileA(tempZip);

        if (!existedBefore) {
            std::filesystem::path marker = targetDir / TEMP_MOD_MARKER_FILE;
            FILE* mf = nullptr;
            if (fopen_s(&mf, marker.string().c_str(), "wb") == 0 && mf) {
                fwrite("temp", 1, 4, mf);
                fclose(mf);
            }
        }

        LockMods();
        g_sessionSandboxForwards.push_back({ modId, targetDir.string() });
        UnlockMods();
        if (ioAnyNewOnDisk) *ioAnyNewOnDisk = true;

        Logf("republicmp  MOD_SYNC: [%s] unpacked mod %llu into session sandbox %s",
             roleTag, (unsigned long long)modId, targetDir.string().c_str());
    }
    return true;
}

void ModSync_BeforeWorldLoad(bool isHostStart, const char* saveDir)
{
    if (isHostStart) {
        ModSync_RefreshCatalog();
        std::vector<uint64_t> saveIds;
        if (saveDir && saveDir[0]) {
            InspectSaveModsInternal(saveDir, saveIds);
        }

        // If the save references Workshop mods that the Host doesn't have installed yet,
        // automatically subscribe & download them from Steam Workshop before loading the world!
        if (!saveIds.empty()) {
            std::vector<WireModEntry> saveModEntries;
            for (uint64_t id : saveIds) {
                WireModEntry wm = {};
                wm.id = id;
                wm.isWip = 0;
                wm.itemType = 1;
                _snprintf_s(wm.title, sizeof(wm.title), _TRUNCATE, "Save Mod #%llu", (unsigned long long)id);
                saveModEntries.push_back(wm);
            }
            std::vector<uint64_t> unavailIds;
            bool downloadedAny = false;
            DownloadMissingSteamMods(INVALID_SOCKET, saveModEntries, "HOST-SAVE", unavailIds, &downloadedAny, nullptr, 0);
            if (downloadedAny) {
                g_needMainThreadWorkshopRefresh = true;
            }
        }

        void* mgrPtr = SafeGetEngineWorkshopMgr();
        if (g_needMainThreadWorkshopRefresh && mgrPtr && o_workshopRefresh) {
            DiagLog_Printf("MOD_SYNC", "[HOST] Refreshing SOVIET_WORKSHOP_MANAGER after downloading save mods");
            Hook_WorkshopRefresh(mgrPtr, false, false, false);
            ModSync_RefreshCatalog();
            mgrPtr = SafeGetEngineWorkshopMgr();
        }

        LockMods();
        BuildHostActiveModListLocked(saveIds, g_sessionAllowedIds);
        g_sessionFilterActive = true;
        if (mgrPtr) {
            int before = SafeGetWorkshopMgrCount(mgrPtr) + g_hiddenTailCount;
            int kept = SafeFilterAndSortWorkshopMgr(
                mgrPtr,
                g_sessionAllowedIds.empty() ? nullptr : g_sessionAllowedIds.data(),
                (int)g_sessionAllowedIds.size(),
                &g_hiddenTailCount);
            Logf("republicmp  MOD_SYNC: [HOST] pre-filtered SOVIET_WORKSHOP_MANAGER before world load (%d -> %d items, packMode=%d, saveMods=%zu)",
                 before, kept, (int)g_packMode, saveIds.size());
        }
        UnlockMods();
        return;
    }

    void* mgrPtr = SafeGetEngineWorkshopMgr();
    if (g_needMainThreadWorkshopRefresh && mgrPtr && o_workshopRefresh) {
        DiagLog_Printf("MOD_SYNC", "Running scheduled main-thread SOVIET_WORKSHOP_MANAGER::RefreshItems before world load");
        Hook_WorkshopRefresh(mgrPtr, false, false, false);
    } else if (g_sessionFilterActive && mgrPtr) {
        LockMods();
        SafeFilterAndSortWorkshopMgr(
            mgrPtr,
            g_sessionAllowedIds.empty() ? nullptr : g_sessionAllowedIds.data(),
            (int)g_sessionAllowedIds.size(),
            &g_hiddenTailCount);
        UnlockMods();
    }
}

bool ModSync_HostHandshake(SOCKET sock, const char* saveDir, char* errOut, size_t errSize)
{
    ModSync_RefreshCatalog();

    // 1. Collect all Workshop/WIP mods on Host + required by save
    std::vector<uint64_t> saveIds;
    InspectSaveModsInternal(saveDir, saveIds);

    LockMods();
    g_sessionSandboxForwards.clear();
    std::vector<uint64_t> hostActiveIds;
    BuildHostActiveModListLocked(saveIds, hostActiveIds);

    std::vector<WireModEntry> hostWireMods;
    for (uint64_t id : hostActiveIds) {
        WireModEntry wm = {};
        wm.id = id;
        wm.itemType = 1;
        _snprintf_s(wm.title, sizeof(wm.title), _TRUNCATE, "Mod #%llu", (unsigned long long)id);
        for (const auto& m : g_mods) {
            if (m.id == id) {
                wm.isWip = m.isWip ? 1 : 0;
                wm.itemType = (uint8_t)m.itemType;
                strncpy_s(wm.title, sizeof(wm.title), m.title, _TRUNCATE);
                break;
            }
        }
        bool diskWip = false;
        char folder[MAX_PATH] = {};
        if (FindLocalModFolder(id, folder, sizeof(folder), &diskWip)) {
            wm.isWip = diskWip ? 1 : 0;
        }
        hostWireMods.push_back(wm);
    }

    // Collect enabled TesmioLoader gameplay plugins & their .ini contents
    std::string enabledCsv;
    struct PluginPayload {
        WirePluginIniHeader hdr;
        std::vector<BYTE> data;
    };
    std::vector<PluginPayload> pluginPayloads;

    for (const auto& pe : g_plugins) {
        if (!pe.localEnabled) continue;
        if (!enabledCsv.empty()) enabledCsv += ",";
        enabledCsv += pe.id;

        FILE* f = nullptr;
        if (pe.iniPath[0] && fopen_s(&f, pe.iniPath, "rb") == 0 && f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz <= 256 * 1024) {
                PluginPayload pp = {};
                strncpy_s(pp.hdr.id, sizeof(pp.hdr.id), pe.id, _TRUNCATE);
                pp.hdr.byteSize = (uint32_t)sz;
                pp.data.resize((size_t)sz);
                fread(pp.data.data(), 1, (size_t)sz, f);
                pluginPayloads.push_back(std::move(pp));
            }
            fclose(f);
        }
    }
    ModPackMode packMode = g_packMode;
    UnlockMods();

    DiagLog_Printf("MOD_SYNC", "[HOST] Sending manifest: %zu host mod(s) (saveMods=%zu, packMode=%d), %zu plugin(s) [%s]",
                   hostWireMods.size(), saveIds.size(), (int)packMode, pluginPayloads.size(), enabledCsv.c_str());

    // 2. Send WireModManifestHeader + hostWireMods + pluginPayloads
    WireModManifestHeader hdr = {};
    memcpy(hdr.magic, "RPMPMOD2", 8);
    hdr.packMode = (uint32_t)packMode;
    hdr.modCount = (uint32_t)hostWireMods.size();
    hdr.pluginCount = (uint32_t)pluginPayloads.size();
    strncpy_s(hdr.enabledPluginsCsv, sizeof(hdr.enabledPluginsCsv), enabledCsv.c_str(), _TRUNCATE);

    if (!NetworkSendExact(sock, &hdr, sizeof(hdr))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection while sending mod manifest");
        return false;
    }
    if (!hostWireMods.empty() && !NetworkSendExact(sock, hostWireMods.data(), hostWireMods.size() * sizeof(WireModEntry))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection while sending mod entries");
        return false;
    }
    for (const auto& pp : pluginPayloads) {
        if (!NetworkSendExact(sock, &pp.hdr, sizeof(pp.hdr)) ||
            (pp.hdr.byteSize > 0 && !NetworkSendExact(sock, pp.data.data(), pp.hdr.byteSize))) {
            _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection while sending plugin configs");
            return false;
        }
    }

    // 3. Receive Client's additional mods (when MOD_PACK_ALL_SHARED is active)
    WireClientModListHeader clientHdr = {};
    if (!NetworkRecvExact(sock, &clientHdr, sizeof(clientHdr)) ||
        memcmp(clientHdr.magic, "RPMPCMDS", 8) != 0 || clientHdr.extraModCount > 4096) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving client mod list");
        return false;
    }

    std::vector<WireModEntry> clientExtraMods(clientHdr.extraModCount);
    if (clientHdr.extraModCount > 0 &&
        !NetworkRecvExact(sock, clientExtraMods.data(), clientHdr.extraModCount * sizeof(WireModEntry))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving client extra mod entries");
        return false;
    }

    std::vector<WireModEntry> sessionMods = hostWireMods;
    if (packMode == MOD_PACK_ALL_SHARED) {
        for (const auto& cm : clientExtraMods) {
            bool exists = false;
            for (const auto& hm : sessionMods) {
                if (hm.id == cm.id) { exists = true; break; }
            }
            if (!exists) sessionMods.push_back(cm);
        }
    }

    DiagLog_Printf("MOD_SYNC", "[HOST] Combined session mod pool: %zu mod(s) (%zu from host/save, %u extra from client)",
                   sessionMods.size(), hostWireMods.size(), clientHdr.extraModCount);

    // 4. Auto-download any missing Workshop mods on Host (from save or from Client's extra mods!)
    bool hostAnyNewModsOnDisk = false;
    std::vector<uint64_t> hostNeedFromClient;
    if (!DownloadMissingSteamMods(sock, sessionMods, "HOST", hostNeedFromClient, &hostAnyNewModsOnDisk, errOut, errSize)) {
        return false;
    }

    // 5. Wait for Client's fallback request & stream requested Host mods to Client
    SyncBlock_SetStep(2, 7, Tr("Ожидание проверки и загрузки модов у партнёра...",
                               "Waiting for partner to verify & download mods..."), 0);

    WireModFallbackReq clientReq = {};
    if (!NetworkRecvExact(sock, &clientReq, sizeof(clientReq)) ||
        memcmp(clientReq.magic, "RPMPMREQ", 8) != 0 || clientReq.requestCount > 2048) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection while waiting for client mod verification");
        return false;
    }
    std::vector<uint64_t> clientNeedFromHost(clientReq.requestCount);
    if (clientReq.requestCount > 0 &&
        !NetworkRecvExact(sock, clientNeedFromHost.data(), clientReq.requestCount * sizeof(uint64_t))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection while receiving client fallback mod list");
        return false;
    }
    if (!StreamRequestedModZips(sock, clientNeedFromHost, "HOST", errOut, errSize)) {
        return false;
    }

    // 6. Send Host's fallback request & receive any local/unavailable Client mods from Client
    WireModFallbackReq hostReq = {};
    memcpy(hostReq.magic, "RPMPMREQ", 8);
    hostReq.requestCount = (uint32_t)hostNeedFromClient.size();
    if (!NetworkSendExact(sock, &hostReq, sizeof(hostReq)) ||
        (!hostNeedFromClient.empty() &&
         !NetworkSendExact(sock, hostNeedFromClient.data(), hostNeedFromClient.size() * sizeof(uint64_t)))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection sending host fallback mod request");
        return false;
    }
    if (!ReceiveRequestedModZips(sock, hostNeedFromClient, "HOST", &hostAnyNewModsOnDisk, errOut, errSize)) {
        return false;
    }

    // 7. Activate combined session mod filter & schedule main-thread RefreshItems if new mods arrived
    LockMods();
    g_sessionAllowedIds.clear();
    for (const auto& sm : sessionMods) {
        g_sessionAllowedIds.push_back(sm.id);
    }
    std::sort(g_sessionAllowedIds.begin(), g_sessionAllowedIds.end());
    g_sessionFilterActive = true;

    if (void* mgrPtr = SafeGetEngineWorkshopMgr()) {
        int kept = SafeFilterAndSortWorkshopMgr(
            mgrPtr,
            g_sessionAllowedIds.empty() ? nullptr : g_sessionAllowedIds.data(),
            (int)g_sessionAllowedIds.size(),
            &g_hiddenTailCount);
        if (hostAnyNewModsOnDisk || kept < (int)g_sessionAllowedIds.size()) {
            g_needMainThreadWorkshopRefresh = true;
        }
    }
    UnlockMods();

    Logf("republicmp  MOD_SYNC: [HOST] bidirectional mod handshake complete (%zu total mods, %u client->host, %u host->client, refreshOnLoad=%d)",
         sessionMods.size(), hostReq.requestCount, clientReq.requestCount, (int)g_needMainThreadWorkshopRefresh);
    return true;
}

bool ModSync_ClientHandshake(SOCKET sock, char* errOut, size_t errSize)
{
    WireModManifestHeader hdr = {};
    if (!NetworkRecvExact(sock, &hdr, sizeof(hdr)) || memcmp(hdr.magic, "RPMPMOD2", 8) != 0 ||
        hdr.modCount > 4096 || hdr.pluginCount > 64) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "invalid or incompatible mod manifest from host");
        return false;
    }
    hdr.enabledPluginsCsv[sizeof(hdr.enabledPluginsCsv) - 1] = '\0';

    std::vector<WireModEntry> hostWireMods(hdr.modCount);
    if (hdr.modCount > 0 && !NetworkRecvExact(sock, hostWireMods.data(), hdr.modCount * sizeof(WireModEntry))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving mod list from host");
        return false;
    }

    DiagLog_Printf("MOD_SYNC", "[CLIENT] Received manifest from host: %u mod(s), packMode=%u, %u plugin config(s), enabledPlugins=[%s]",
                   hdr.modCount, hdr.packMode, hdr.pluginCount, hdr.enabledPluginsCsv);

    char rootDir[MAX_PATH] = {};
    GetExeRootDir(rootDir, sizeof(rootDir));
    std::error_code ec;
    std::filesystem::path sessionIniDir = std::filesystem::path(rootDir) / "plugins" / ".mp_session_ini";
    std::filesystem::remove_all(sessionIniDir, ec);
    std::filesystem::create_directories(sessionIniDir, ec);

    for (uint32_t i = 0; i < hdr.pluginCount; ++i) {
        WirePluginIniHeader ph = {};
        if (!NetworkRecvExact(sock, &ph, sizeof(ph)) || ph.byteSize > 256 * 1024) {
            _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving plugin config header");
            return false;
        }
        ph.id[sizeof(ph.id) - 1] = '\0';
        std::vector<BYTE> data(ph.byteSize);
        if (ph.byteSize > 0 && !NetworkRecvExact(sock, data.data(), ph.byteSize)) {
            _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving plugin config data");
            return false;
        }
        std::filesystem::path iniFile = sessionIniDir / (std::string(ph.id) + ".ini");
        FILE* f = nullptr;
        if (fopen_s(&f, iniFile.string().c_str(), "wb") == 0 && f) {
            if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
            fclose(f);
        }
    }

    // Apply session plugin override in tesmioloader.dll
    if (!g_pluginMgr && g_host && g_host->consume) {
        g_pluginMgr = (const TsmPluginManagerApi*)g_host->consume(TSM_SERVICE_PLUGINS, TSM_PLUGINS_VERSION);
    }
    if (g_pluginMgr) {
        g_pluginMgr->setSessionOverride(sessionIniDir.string().c_str(), hdr.enabledPluginsCsv);
    }

    // Send Client's additional installed Workshop/WIP mods to Host when in MOD_PACK_ALL_SHARED mode
    ModSync_RefreshCatalog();
    std::vector<WireModEntry> clientExtraMods;
    LockMods();
    g_sessionSandboxForwards.clear();
    if (hdr.packMode == (uint32_t)MOD_PACK_ALL_SHARED) {
        for (const auto& m : g_mods) {
            if (m.itemType == 4) continue; // skip pure local text translation mods
            bool alreadyInHost = false;
            for (const auto& hm : hostWireMods) {
                if (hm.id == m.id) { alreadyInHost = true; break; }
            }
            if (!alreadyInHost) {
                WireModEntry wm = {};
                wm.id = m.id;
                wm.isWip = m.isWip ? 1 : 0;
                wm.itemType = (uint8_t)m.itemType;
                strncpy_s(wm.title, sizeof(wm.title), m.title, _TRUNCATE);
                clientExtraMods.push_back(wm);
            }
        }
    }
    UnlockMods();

    WireClientModListHeader clientHdr = {};
    memcpy(clientHdr.magic, "RPMPCMDS", 8);
    clientHdr.extraModCount = (uint32_t)clientExtraMods.size();
    if (!NetworkSendExact(sock, &clientHdr, sizeof(clientHdr)) ||
        (!clientExtraMods.empty() &&
         !NetworkSendExact(sock, clientExtraMods.data(), clientExtraMods.size() * sizeof(WireModEntry)))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection sending client mod list to host");
        return false;
    }

    std::vector<WireModEntry> sessionMods = hostWireMods;
    for (const auto& cm : clientExtraMods) {
        sessionMods.push_back(cm);
    }

    // Auto-download any missing Workshop mods from Host
    bool clientAnyNewModsOnDisk = false;
    std::vector<uint64_t> clientNeedFromHost;
    if (!DownloadMissingSteamMods(sock, hostWireMods, "CLIENT", clientNeedFromHost, &clientAnyNewModsOnDisk, errOut, errSize)) {
        return false;
    }

    // Send Client's fallback request & receive requested Host mods
    WireModFallbackReq clientReq = {};
    memcpy(clientReq.magic, "RPMPMREQ", 8);
    clientReq.requestCount = (uint32_t)clientNeedFromHost.size();
    if (!NetworkSendExact(sock, &clientReq, sizeof(clientReq)) ||
        (!clientNeedFromHost.empty() &&
         !NetworkSendExact(sock, clientNeedFromHost.data(), clientNeedFromHost.size() * sizeof(uint64_t)))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection sending client fallback mod request");
        return false;
    }
    if (!ReceiveRequestedModZips(sock, clientNeedFromHost, "CLIENT", &clientAnyNewModsOnDisk, errOut, errSize)) {
        return false;
    }

    // Receive Host's fallback request & stream any requested Client mods to Host
    WireModFallbackReq hostReq = {};
    if (!NetworkRecvExact(sock, &hostReq, sizeof(hostReq)) ||
        memcmp(hostReq.magic, "RPMPMREQ", 8) != 0 || hostReq.requestCount > 2048) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving host fallback mod request");
        return false;
    }
    std::vector<uint64_t> hostNeedFromClient(hostReq.requestCount);
    if (hostReq.requestCount > 0 &&
        !NetworkRecvExact(sock, hostNeedFromClient.data(), hostReq.requestCount * sizeof(uint64_t))) {
        _snprintf_s(errOut, errSize, _TRUNCATE, "lost connection receiving host fallback mod IDs");
        return false;
    }
    if (!StreamRequestedModZips(sock, hostNeedFromClient, "CLIENT", errOut, errSize)) {
        return false;
    }

    // Activate combined session mod filter and schedule main-thread RefreshItems if new mods arrived
    LockMods();
    g_sessionAllowedIds.clear();
    for (const auto& sm : sessionMods) {
        g_sessionAllowedIds.push_back(sm.id);
    }
    std::sort(g_sessionAllowedIds.begin(), g_sessionAllowedIds.end());
    g_sessionFilterActive = true;

    if (void* mgrPtr = SafeGetEngineWorkshopMgr()) {
        int kept = SafeFilterAndSortWorkshopMgr(
            mgrPtr,
            g_sessionAllowedIds.empty() ? nullptr : g_sessionAllowedIds.data(),
            (int)g_sessionAllowedIds.size(),
            &g_hiddenTailCount);
        if (clientAnyNewModsOnDisk || kept < (int)g_sessionAllowedIds.size()) {
            g_needMainThreadWorkshopRefresh = true;
        }
    }
    UnlockMods();

    Logf("republicmp  MOD_SYNC: [CLIENT] bidirectional mod handshake complete (%zu active session mods, plugins=\"%s\", refreshOnLoad=%d)",
         sessionMods.size(), hdr.enabledPluginsCsv, (int)g_needMainThreadWorkshopRefresh);
    return true;
}

void ModSync_EndSession()
{
    bool wasActive = false;
    bool hadSandboxMods = false;
    LockMods();
    if (g_sessionFilterActive || !g_sessionSandboxForwards.empty() || g_hiddenTailCount > 0) {
        wasActive = true;
        hadSandboxMods = !g_sessionSandboxForwards.empty();
        g_sessionFilterActive = false;
        g_sessionAllowedIds.clear();
        g_sessionSandboxForwards.clear();
        if (void* mgrPtr = SafeGetEngineWorkshopMgr()) {
            SafeRestoreWorkshopMgr(mgrPtr, &g_hiddenTailCount);
        }
        if (hadSandboxMods) {
            g_needMainThreadWorkshopRefresh = true;
        }
    }
    UnlockMods();

    if (g_pluginMgr) {
        g_pluginMgr->clearSessionOverride();
    }

    if (wasActive) {
        CleanupTempSessionMods();
        Logf("republicmp  MOD_SYNC: session ended, singleplayer mods & plugins restored (hadSandbox=%d)",
             (int)hadSandboxMods);
    }
}
