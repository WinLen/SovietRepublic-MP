// SPDX-License-Identifier: GPL-3.0-only
// Same "camera-visible" edges on both PCs for one command.
//
// Every network object (world+0xf7a0 roads, +0xf968 footpaths, ... - the
// managers in road_sync) keeps, besides its committed edge vector
// (+0x2b0/+0x2b8), two lists the renderer fills each frame with the edges
// the local camera sees: +0xaa0/+0xaa8/+0xab0 and +0xab8/+0xac0/+0xac8. The
// road planner, its picks and a building's automatic access road read
// them, so the same command replayed on the other PC found nothing to
// demolish when that player looked elsewhere, or laid another access road
// (a footpath on one PC, a road on the other).
//
// ViewListScope_Begin puts, into both lists of every network, the edges of
// the committed vector with a node or a point inside the box - in the
// committed vector's order, so both PCs see the same lists. The buffers come
// from the game's own array allocator (FUN_1400019e0 / FUN_1400001a50), so a
// list the command grows is freed correctly. ViewListScope_End puts each
// PC's own list back without the edges the command removed (Late's tools
// read them before the renderer refills them), with the edges the command
// made added (a cut's halves: the tools pick through these lists). The renderer lock
// ([exe+0x995268]+0x80, held by the main thread's Early) is held from Begin
// to End.
//
// (by SoylentGreed)
#pragma once

#include "tesmio_plugin.h"

void ViewListScope_Bind(const TsmHost* host);

// XZ box. Returns true if the lists were replaced (call ViewListScope_End);
// false leaves everything native (no lock, lists unreadable). Nested calls
// count up and keep the outer box.
bool ViewListScope_Begin(float minX, float minZ, float maxX, float maxZ);
void ViewListScope_End();
