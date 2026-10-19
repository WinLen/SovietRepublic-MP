// SPDX-License-Identifier: GPL-3.0-only
// IAT hooks on C3D_TERRAIN: every heightmap / mask edit the exe asks
// C3DDLL64 to make passes through here. Each call is logged with its caller
// RVA and, for the edits that are synced, handed to world_sync/terrain/terrain_sync.
//
// What is hooked (SOVIET64.exe IAT, signatures from C3DDLL64.dll exports)
//   EditRaiseLower(vec3, f, f, f, f, bool)
//   EditRaiseLowerAngleLimit(vec3, f, f, f, f, bool, f)
//   EditRaiseLowerAngleLimitGetCost(vec3, f, f, f, f, bool) -> f   (read-only)
//   EditSmooth(vec3, f, f, f, bool)
//   EditLevel(vec3, f, f, f, bool)
//   EditToHeight(vec3, vec3, f, f, f, bool)
//   EditSmoothWithLimits(vec3, f x9)
//   EditMask(vec3, int, f, f, int, int, bool)
//   EditMaskGen(int, f x6)
//   EditMaskGenClear(int)
//   HeightmapTextureOpen/Close, MaskTextureOpen/Close
//   RegenerateCollisionDataSoviet, RegenerateCollisionNormalDataSoviet,
//   SetTerrainHeight(f)
// A C3DVECTOR3 argument is 12 bytes, so the x64 ABI passes it as a pointer
// to a caller-made copy - the hooks take `const float*` and forward it as is.
//
// Synced edits: EditRaiseLower, EditRaiseLowerAngleLimit, EditSmooth,
// EditLevel, EditToHeight, EditMask. The rest only run during map
// generation / load (see kKnownCallers) and are logged, never sent.
//
// Edits come in bursts (one call per brush point, many per frame), so by
// default the log is off; F8 cycles it: off -> aggregated per frame by
// (method, caller), flushed from TerrainHook_Tick() -> one line per call.
// Calls made inside the C3DDLL64 itself do not go through the exe's IAT and
// are not seen.
#pragma once

#include "tesmio_plugin.h"

enum TerrainMethod {
    TM_RAISE_LOWER,
    TM_RAISE_LOWER_ANGLE,
    TM_RAISE_LOWER_ANGLE_COST,
    TM_SMOOTH,
    TM_LEVEL,
    TM_TO_HEIGHT,
    TM_SMOOTH_LIMITS,
    TM_MASK,
    TM_MASK_GEN,
    TM_MASK_GEN_CLEAR,
    TM_HEIGHTMAP_OPEN,
    TM_HEIGHTMAP_CLOSE,
    TM_MASK_OPEN,
    TM_MASK_CLOSE,
    TM_REGEN_COLLISION,
    TM_REGEN_COLLISION_NORMAL,
    TM_SET_TERRAIN_HEIGHT,
    TM_COUNT
};

// Raw arguments of one call. Only the fields the method has are filled;
// float/int order is the order of the method's parameters.
struct TerrainArgs {
    float pos[3];
    float pos2[3];
    float f[9];
    int i[3];
    int b;
    float ret;
    unsigned char hasPos, hasPos2, nf, ni, hasB, hasRet;
    unsigned char suppressed; // logging only: the call was swallowed (see terrain_sync.h)
};

void TerrainHook_Bind(const TsmHost* host);
bool InstallTerrainHooks();

// Per-frame (game thread, Present hook): flushes the aggregated calls of the
// frame that just ended and polls the F8 log-mode key.
void TerrainHook_Tick();
// The terrain call log: 0 off, 1 aggregated per frame, 2 one line per call.
void TerrainHook_SetLogMode(int mode);
int TerrainHook_GetLogMode();

// The live C3D_TERRAIN object, *(*(exe+0x9941F0)+0xED8), or nullptr before a
// world is loaded.
void* TerrainHook_GetTerrain();

// Calls the ORIGINAL engine functions (not the hooks), for replaying remote
// edits. ApplyEdit returns false for a method that is not a synced edit.
bool TerrainHook_ApplyEdit(void* terrain, int method, const TerrainArgs& args);
void TerrainHook_HeightmapOpen(void* terrain);
void TerrainHook_HeightmapClose(void* terrain);
void TerrainHook_MaskOpen(void* terrain);
void TerrainHook_MaskClose(void* terrain);

// Lockstep (lockstep/commands): while commands are captured, an edit a tool
// makes on this PC is not made: it is collected, Flush (main thread, start of
// a frame) hands the collected edits over as one command, and ApplyCommand
// makes them on every PC through the original engine functions, inside the
// heightmap (and mask) open/close - as a remote batch of the replay
// multiplayer is applied. Edits the simulation or a command makes run as
// usual.
void TerrainHook_LockstepFlush();
bool TerrainHook_ApplyCommand(const void* data, size_t size);

// World-space terrain height under pos (CollisionTerrainHeightOverPoint).
// Returns false if the engine function or terrain is unavailable.
bool TerrainHook_HeightAt(const float pos[3], float* outHeight);

// Set while terrain_sync replays a remote batch: the hooks then run the
// original and do not capture the edit, so it is never sent back.
extern volatile long g_terrainApplyingRemote;
