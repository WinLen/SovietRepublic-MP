// SPDX-License-Identifier: GPL-3.0-only
// Camera-independent building placement: an inline hook on FUN_140569d70,
// the network collision query.
//
// The building tool (FUN_1402bade0) checks its spot and lays its automatic
// access road with 19 direct calls of this query, and two of its inputs come
// from what the local camera sees: argument 10 (byte 1 = test only the
// camera-visible edges, network+0xaa0) and a shortlist the tool fills from
// the visible edges first (network+0xb00/+0xb08, FUN_14056ddd0) - a
// nonempty shortlist replaces the whole network regardless of argument 10.
// With the two cameras in different places the same placement therefore
// tested different roads: the access road took another route on the other
// PC (a duplicate next to the sender's, or one the sender's demolition
// couldn't find), or the building was refused there.
//
// For those 19 call sites only (identified by return address) the hook runs
// the query against the whole network: argument 10 cleared and the
// shortlist's end set to its begin for the call, then restored. Every other
// caller (194 in the road planner alone) gets the native query untouched.
//
// (by SoylentGreed)
#pragma once

#include "tesmio_plugin.h"

#define HOOK_COLLISION_QUERY_RVA 0x569d70

void PlacementCollision_Bind(const TsmHost* host);
bool InstallPlacementCollisionHook();
