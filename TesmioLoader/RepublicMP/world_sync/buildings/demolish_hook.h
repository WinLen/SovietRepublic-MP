// SPDX-License-Identifier: GPL-3.0-only
// Building demolition detection: an inline hook on FUN_140168730, the
// engine's "demolish this building" function.
//
// FUN_140168730(thisPtr, building, p3, p4, p5, p6, p7) - p3/p4 in r8b/r9b,
// p5..p7 on the stack (byte flags; the callee reads only the low byte, the
// hook forwards the raw qwords). Player paths into it:
//   - terrain_demolish tool with the BUILDING_SLOW_ROTATE modifier held
//     (FUN_1403abc90, 4 call sites) - demolishes without the prompt;
//   - the "building_demolish_promt" confirmation button (FUN_1407fc880,
//     calls it with [dialog+0x240] = the building).
// Both write building+0xeb1 = 0 just before the call. It is also called from
// ~8 other places (simulation, other UI) and recursively for attached
// sub-buildings (FUN_140168730 itself).
//
// What is sent: every OUTERMOST call made by local play - not the recursive
// ones, not calls inside a building placement (the placement replay redoes
// those), not calls made while replaying remote work. The building is
// identified by position (obj+0x350) and type category (*(obj+0x318)+0x360);
// the receiver demolishes its own building there with the same flags
// (world_sync/buildings/build_sync, BUILD_SYNC_KIND_DEMOLISH). A building already gone on the
// receiver is skipped, which also makes a demolition both machines' own
// simulations decide on harmless.
#pragma once

#include "tesmio_plugin.h"

#define HOOK_DEMOLISH_RVA 0x168730

void DemolishHook_Bind(const TsmHost* host);
bool InstallDemolishHook();

// Per-frame (game thread, Present hook, after TerrainSync_Tick): sends the
// demolitions captured since the last frame.
void DemolishHook_Tick();

// True while FUN_140168730 is running (local or remote). Road commits made
// inside it are the demolition's own side effects and are not sent.
bool DemolishHook_InDemolish();

struct BuildSyncMsg;

// Calls the original (unhooked) function, for replaying a remote demolition.
// Applies the sender's building+0xeb1, Shift/Ctrl modifiers, BuildMode, dialog 0x424
// settings, and demolition office tool assignment. Returns false if it threw.
bool DemolishHook_CallOriginal(__int64 thisPtr, __int64 building, const BuildSyncMsg& msg);
