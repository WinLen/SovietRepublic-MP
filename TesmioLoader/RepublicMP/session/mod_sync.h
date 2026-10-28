// SPDX-License-Identifier: GPL-3.0-only
// Host-Authoritative Steam Workshop Mod Synchronization for RepublicMP Co-op:
// - Inspects save header.bin / headerE.bin to find Workshop items used in a save.
// - Pre-save handshake executes before world save: Host transmits required Workshop
//   manifest, Client checks disk. If missing mods exist, Client starts non-blocking
//   batch ISteamUGC download, Host stays in SESSION_HOST_LISTENING without saving/packing.
// - In-game Soviet UI progress modal displays batch download status; at 100%, offers
//   one-click game restart (ModSync_RestartGameNow) to cleanly re-initialize engine type tables.
// - Strict Dual-Layer Ban:
//     * Layer 1: In-place partition & sort of SOVIET_WORKSHOP_MANAGER vector (WORKSHOP_MGR_PTR_RVA 0x9E22B8),
//       keeping allowed IDs + text localization (itemType 4), restoring on session end.
//     * Layer 2: Action hook guards in local_builds.cpp and purchase.cpp rejecting uninstalled
//       Workshop items with 1500ms debounced localized red toasts.
#pragma once

#include "tesmio_plugin.h"
#include "../transport/network.h"
#include <stdint.h>

enum ModPackMode {
    MOD_PACK_HOST_ONLY = 0, // Default: All installed Host mods + mods in save
    MOD_PACK_SAVE_ONLY = 1, // Only mods referenced by the save
};

enum ModSyncHostResult {
    MOD_SYNC_HOST_OK = 0,
    MOD_SYNC_HOST_CLIENT_MISSING = 1,
    MOD_SYNC_HOST_ERROR = 2,
};

enum ModSyncClientResult {
    MOD_SYNC_CLIENT_OK = 0,
    MOD_SYNC_CLIENT_MISSING = 1,
    MOD_SYNC_CLIENT_ERROR = 2,
};

struct ModDownloadProgress {
    bool     active;
    bool     allCompleted;
    int      completedCount;
    int      totalCount;
    char     currentTitle[128];
    uint64_t currentBytesDownloaded;
    uint64_t currentBytesTotal;
    uint64_t totalBytesDownloaded;
    uint64_t totalBytesTotal;
};

void ModSync_Bind(const TsmHost* host);
void ModSync_Init();

// Refreshes the local catalog of Workshop/WIP mods and scans the latest coop save's header.bin.
void ModSync_RefreshCatalog();

// Server Mod Auto-Sync Mode (persisted in republicmp_settings.ini)
ModPackMode ModSync_GetPackMode();
void        ModSync_SetPackMode(ModPackMode mode);
void        ModSync_CyclePackMode();

// Diagnostic counters
int ModSync_GetModCount();
int ModSync_GetActiveServerModCount();

// Persistence hooks called by TwoStates_LoadSettings / TwoStates_SaveSettings
void ModSync_LoadSettingsFromIni(const char* iniPath);
void ModSync_SaveSettingsToIni(const char* iniPath);

// Called on the main thread inside TwoStates_BeforeAnyWorldLoad right before o_loadWorld:
// - If isHostStart is true, auto-downloads any missing Workshop mods referenced by saveDir
//   and activates the Host's server mod filter.
// - If new mods were downloaded, refreshes SOVIET_WORKSHOP_MANAGER safely on the main thread.
void ModSync_BeforeWorldLoad(bool isHostStart, const char* saveDir);

// Pre-save handshake between Host and Client (called inside RunHostJoin / RunClientJoin
// right after DlcsMatch, before the shared world save is created).
ModSyncHostResult   ModSync_HostHandshake(SOCKET sock, const char* saveDir, char* errOut, size_t errSize);
ModSyncClientResult ModSync_ClientHandshake(SOCKET sock, char* errOut, size_t errSize);

// Download progress and restart controls
bool ModSync_GetDownloadProgress(ModDownloadProgress* outProgress);
void ModSync_TickDownloadProgress();
void ModSync_RestartGameNow();
void ModSync_CancelDownloadModal();
bool ModSync_IsDownloadActive();

// Layer 2 action hook query predicates
bool ModSync_IsModIdAllowedInSession(uint64_t modId);
bool ModSync_CheckBuildingTypeAllowed(const char* typeName);
bool ModSync_CheckVehicleTypeAllowed(const void* vehicleTypePtr);

// Called when a Co-op session ends or returns to the main menu:
// clears the session mod filter and restores singleplayer mods & plugins.
void ModSync_EndSession();

// Testable domain helpers (used by runtime and unit test suite)
void ModSync_ExtractHeaderBinModIdsFromBuffer(const void* data, size_t size, uint64_t* outIds, int maxOut, int* ioCount);
int  ModSync_FilterAndSortWorkshopMgrForTest(void* mgrPtr, const uint64_t* allowedIds, int allowedCount, int* ioHiddenTail);
void ModSync_RestoreWorkshopMgrForTest(void* mgrPtr, int* ioHiddenTail);
void ModSync_SetTestSessionFilter(bool active, const uint64_t* allowedIds, int count);

