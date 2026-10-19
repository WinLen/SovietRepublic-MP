// SPDX-License-Identifier: GPL-3.0-only
// Terrain sync: network replication of every heightmap / mask edit.
//
// What travels
// ------------
// Not the texture - the CALLS. world_sync/terrain/terrain_hook sees every synced edit the
// exe makes (EditRaiseLower, EditRaiseLowerAngleLimit, EditSmooth, EditLevel,
// EditToHeight, EditMask) with its raw arguments. The strength argument is
// frame-time dependent (C3D_TIMER::Power is multiplied into it at the call
// site), so the arguments are replayed exactly as captured, never
// recomputed. Running the same C3DDLL64 code on the same arguments rebuilds
// the heightmap AND its collision, which is why the calls are sent rather
// than pixels.
//
// One batch per frame: all edits the local game made during the frame, in
// call order. The receiver applies a batch inside HeightmapTextureOpen/Close
// (and MaskTextureOpen/Close if it has mask edits) through the original
// engine functions, with g_terrainApplyingRemote set so the hooks neither
// capture nor resend it. Each batch also carries the sender's terrain height
// at a few edited points after the frame; the receiver compares after
// applying and logs any drift.
//
// Who edits terrain on the receiver
// ---------------------------------
// While connected, ONLY this channel. Road and building replays
// (world_sync/networks/road_sync, world_sync/buildings/build_sync) run inside
// TerrainSync_Begin/EndForeignReplay: their own terrain edits are swallowed
// (the sender's edits arrive here instead), and the mask-paint entries they
// queue (thisPtr+0x130d0, drained by FUN_14040ece0 into EditMask) are cut
// off again - the sender's EditMask calls arrive here too. Without that,
// every road/building would deform the receiver's terrain twice.
//
// Money is not touched: a raw Edit* call charges nothing; the player who
// made the edit paid for it on their own machine.
//
// Transport: one TCP connection (TERRAIN_SYNC_PORT), symmetric host<->client.
// Sending happens on a worker thread, so a slow network never stalls a
// frame. Edits made while not connected are not queued - the save sync that
// starts every session carries the terrain.
//
// Both machines must run the same build - the wire format is not versioned.
#pragma once

#include "tesmio_plugin.h"
#include "../../core/host_config.h"
#include "terrain_hook.h"

#define TERRAIN_SYNC_PORT 5559

// Points per batch whose height is compared on the receiver.
#define TERRAIN_SYNC_MAX_CHECKS 8

// Upper bound on one batch's encoded edits; a busier frame is split.
#define TERRAIN_SYNC_MAX_PAYLOAD (64 * 1024)

struct TerrainSyncCheck {
    float pos[3];
    float height; // sender's CollisionTerrainHeightOverPoint after the frame
};

// Wire header of one batch; followed by the encoded edits - payloadBytes of
// them raw, or compressedBytes of zlib (miniz) data that inflate to
// payloadBytes. Consecutive brush edits repeat most of their floats, so a
// busy batch shrinks several-fold.
// Edit encoding: u8 method, u8 bool, then the method's fields in order -
// pos[3] (all synced methods), pos2[3] (EditToHeight), floats, ints. Field
// counts are fixed per method (kWireLayout in terrain_sync.cpp).
struct TerrainSyncBatchHeader {
    int seq;
    int editCount;
    int payloadBytes;
    int compressedBytes; // 0 = payload sent raw
    int checkCount;
    TerrainSyncCheck checks[TERRAIN_SYNC_MAX_CHECKS];
};

#define TERRAIN_SYNC_WIRE_BATCH 1

void TerrainSync_Bind(const TsmHost* host);

// From world_sync/terrain/terrain_hook (game thread): a synced edit that really ran
// locally. Ignored while not connected.
void TerrainSync_CaptureEdit(int method, const TerrainArgs& args);

// From world_sync/terrain/terrain_hook: true while a foreign replay is running and the
// terrain channel is connected - the edit must not run.
bool TerrainSync_SuppressLocalEdits();

// Bracket a replay of remote roads/buildings (game thread). Nestable.
void TerrainSync_BeginForeignReplay();
void TerrainSync_EndForeignReplay();

// Causality fence between channels. A road/building/demolish message is
// stamped with TerrainSync_GetFenceSeq() when it is sent (game thread) - the
// seq of the last terrain batch that holds edits made before it, including
// the batch still being filled this frame. The receiver applies the message
// only once TerrainSync_FenceReached(stamp): the edits it was built on (e.g.
// the RMB flattening under a road) are in place first. Each channel is its
// own TCP connection, so without this a road can overtake its terrain.
int TerrainSync_GetFenceSeq();
bool TerrainSync_FenceReached(int fence);

// Per-frame (game thread, Present hook): sends the frame's edits, then
// applies received batches in order once a world is loaded.
void TerrainSync_Tick();

// Connection control (UI / save-sync flow).
void TerrainSync_StartHost();
void TerrainSync_StartClient(const char* hostIp = nullptr); // nullptr = keep last IP
void TerrainSync_Reset();

bool TerrainSync_IsConnected();
bool TerrainSync_IsConnecting();
bool TerrainSync_IsHostRole();
const char* TerrainSync_GetTargetIp();
