// SPDX-License-Identifier: GPL-3.0-only
// Automatic Bidirectional Mod & Plugin Synchronization for RepublicMP Co-op:
// - Inspects save header.bin / headerE.bin / tesmioloader.save.ini to find
//   Workshop buildings & vehicles used in a save.
// - Hooks SOVIET_WORKSHOP_MANAGER::RefreshItems (RVA 0x8407C0) to catalog
//   installed Workshop & workshop_wip mods and sort/filter the active mod list
//   during a Co-op session so both players have identical type tables.
// - Automatically enables all installed Workshop/WIP mods without manual clicking,
//   and bidirectionally auto-subscribes & downloads missing mods on BOTH Host and
//   Client via ISteamUGC (with automatic network archive fallback into a temporary
//   session sandbox for local workshop_wip or unavailable items).
// - Synchronizes TesmioLoader gameplay plugins & .ini configs in a temporary
//   session sandbox (plugins\.mp_session_ini) with hot-reload before LoadWorld.
#pragma once

#include "tesmio_plugin.h"
#include "../transport/network.h"
#include <stdint.h>

enum ModPackMode {
    MOD_PACK_SAVE_ONLY  = 0, // Only mods already used in the save (auto-downloaded by Host & Client)
    MOD_PACK_ALL_SHARED = 1, // Default: All Host + Client + Save mods enabled & auto-downloaded both ways
    MOD_PACK_HOST_ONLY  = 2, // All Host + Save mods enabled (Host downloads missing save mods, Client downloads Host mods)
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
// - If new mods were downloaded/unpacked during handshake, refreshes SOVIET_WORKSHOP_MANAGER
//   safely on the main thread before LoadWorld parses header.bin.
void ModSync_BeforeWorldLoad(bool isHostStart, const char* saveDir);

// Bidirectional handshake between Host and Client (called inside RunHostJoin / RunClientJoin
// right after CheckDlcs, before the shared world save is loaded).
bool ModSync_HostHandshake(SOCKET sock, const char* saveDir, char* errOut, size_t errSize);
bool ModSync_ClientHandshake(SOCKET sock, char* errOut, size_t errSize);

// Called when a Co-op session ends or returns to the main menu:
// clears the session mod filter and restores singleplayer mods & plugins.
void ModSync_EndSession();

// Testable domain helpers (used by runtime and unit test suite)
void ModSync_ExtractHeaderBinModIdsFromBuffer(const void* data, size_t size, uint64_t* outIds, int maxOut, int* ioCount);
int  ModSync_FilterAndSortWorkshopMgrForTest(void* mgrPtr, const uint64_t* allowedIds, int allowedCount, int* ioHiddenTail);
void ModSync_RestoreWorkshopMgrForTest(void* mgrPtr, int* ioHiddenTail);

