// SPDX-License-Identifier: GPL-3.0-only
// Session patches: small code patches that take the camera and the frame
// rate out of the simulation while two players are connected, so both PCs
// run the same branch. Each one is checked against the original bytes first
// (nothing is written unless every site matches), applied when the session
// connects and put back when it ends.
//
//   - Frame rate: the Early building pass FUN_140139a70 updates a
//     third of the buildings per pass with the step tripled. When the real
//     frame rate drops below 6 (speed 1) or 10 (speed 2) - e.g. the game in
//     the background after alt-tab - it switches to updating every building
//     with the plain step, so construction ran differently on the PC that
//     was alt-tabbed. The three `test r12b,r12b / jne` branch points always
//     take the "frame rate normal" path.
//   - Construction sites and the camera: a building with no type
//     (a construction site) copies its render node's visibility into +115a,
//     and its workers are then updated every frame instead of every third
//     - on the PC whose camera sees the site only. The byte read becomes 0,
//     the path vanilla takes for a site off the camera.
//   - Citizen init: a citizen's +18 is set by the simulation only,
//     never first by an interface overlay, window or getter on one PC.
//
// (by SoylentGreed)
#pragma once

#include "tesmio_plugin.h"

void SessionPatches_Bind(const TsmHost* host);

// Per frame (Present hook): applies the patches while connected or while
// the determinism probe runs, restores them otherwise.
void SessionPatches_Tick();

// Returns true if session patches are currently applied to SOVIET64.exe code pages.
bool SessionPatches_IsApplied();

// Restores patches back to original engine bytes and clears applied status.
void SessionPatches_Restore();

// Test helper for unit tests in tests/test_main.cpp.
void SessionPatches_SetAppliedForTest(bool applied);
