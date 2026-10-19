// SPDX-License-Identifier: GPL-3.0-only
// Local build detection: the two engine hooks that observe what the local
// player builds and forward it to the sync modules.
//
// Building placement - FUN_1402bade0 (HOOK_UNIVERSAL_RVA)
//   The game's single placement function. It runs every frame while a
//   building is selected (preview ghost) and creates the building on the
//   confirming click. The hook detects creation by growth of the city
//   buildings vector, picks the new object that sits on the placement cursor
//   (some types add helper objects elsewhere) and sends the placement INPUTS
//   (cursor, yaw, param_5) through world_sync/buildings/build_sync.
//
// Infrastructure commit - FUN_140518a80 (HOOK_ROAD_COMMIT_RVA)
//   The commit shared by every drag tool. On entry, for a known tool manager
//   (world_sync/networks/road_sync RoadSync_CategoryForManager), the hook copies the finished
//   route - interior records, both node positions and building ports - or,
//   for an upgrade commit, the segments being re-typed, and sends it through
//   world_sync/networks/road_sync from the next frame. Commits made inside a building
//   placement (a building laying its own access road) are not sent.
//
// Also provides the F9 debug spawn (a synthetic placement for solo testing)
// and a periodic heartbeat log line.
#pragma once

#include "tesmio_plugin.h"

#define HOOK_UNIVERSAL_RVA 0x2BADE0
#define HOOK_ROAD_COMMIT_RVA 0x518A80

// Expected function prologues for the inline hooks (SOVIET64.exe, 2026-09-07).
extern const unsigned char kUniversalExpect[21];
extern const unsigned char kRoadCommitExpect[19];

typedef void (*UniversalFn)(__int64 param_1, char* param_2, char param_3, char param_4,
                             unsigned __int64 param_5, unsigned int param_6,
                             char param_7, char param_8);
extern UniversalFn g_origUniversal;

typedef void(__fastcall* RoadCommitFn)(__int64 manager,
                                        unsigned __int64 p2, unsigned __int64 p3,
                                        unsigned __int64 p4, unsigned int p5,
                                        unsigned __int64 p6, unsigned __int64 p7);
extern RoadCommitFn g_origRoadCommit;

void HookedUniversal(__int64 param_1, char* param_2, char param_3, char param_4,
                      unsigned __int64 param_5, unsigned int param_6,
                      char param_7, char param_8);

void HookedRoadCommit(__int64 manager,
                      unsigned __int64 p2, unsigned __int64 p3,
                      unsigned __int64 p4, unsigned int p5,
                      unsigned __int64 p6, unsigned __int64 p7);

// Bracket every building-placement call (local or remote replay). Commits
// made inside it are the building's own auto-connections and are not sent.
// Lockstep: the building tool's placing press (sim_patches, 2cc37c). True if
// it became a placement command - the game then reads it as not pressed.
bool LocalBuilds_PlacementPress();

void LocalBuilds_BeginBuildingPlacement();
void LocalBuilds_EndBuildingPlacement();
bool LocalBuilds_InBuildingPlacement();


// Per-frame (game thread, Present hook): tracks the drag-active flag, sends
// captured commits and services F9 requests.
void LocalBuilds_Tick();

// True while the LOCAL player has a drag in progress with any path tool
// (thisPtr+0xd2ac). Remote segments are not applied meanwhile.
bool LocalBuilds_IsToolDragActive();

void LocalBuilds_Bind(const TsmHost* host);

// Lockstep: a network demolition command was made (every PC); the clicking PC
// may send that piece again if the tool still selects it.
struct RoadSyncDemolishMsg;
struct RoadSyncDemolishHover;
void LocalBuilds_DemolishMade(const RoadSyncDemolishMsg& m, const RoadSyncDemolishHover* hover = nullptr);
