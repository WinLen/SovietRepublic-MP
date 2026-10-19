// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the junction window's Accept as a command (by SoylentGreed).
//
// The junction window (758420..75c3a7) edits a working copy (window +270,
// made by 222a50 from the junction at window +240): the kind (+0: 0 the
// right-hand rule, 1 a main road, 2 traffic lights), the node list +88, the
// arms +a0 (0x1d8 each) and the light phases +b8 (12 bytes: arm, arm,
// seconds). crossroad_add and the window's buttons change only the copy.
// Accept (7595bb..7595f9) writes it into the junction:
//   224300(junction)                   clears the vehicles' references to it
//   226530(junction, copy, -)          copies the kind, nodes, arms and phases
//   22a6d0(junction, [world+12808]+38) for traffic lights, else f98d0(junction, same list)
// In lockstep the 226530 call is taken (game_calls) and the copy is read on
// the clicking PC; the other calls of the block are not made there. The copy
// travels with its nodes as places in the road network, and every PC makes the
// block's calls on its own junction at the boundary.
#pragma once

#include "tesmio_plugin.h"

void JunctionAccept_Bind(const TsmHost* host);

// Render thread, at the 226530 call (game_calls): the window's Accept.
void JunctionAccept_Capture(unsigned __int64 junction, unsigned __int64 copy);

// The command (lockstep/commands).
bool JunctionAccept_ApplyCommand(const void* data, size_t size);
