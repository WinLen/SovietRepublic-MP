// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the terrain brushes and single trees as commands (by SoylentGreed).
//
// The Late dispatcher calls the brush tools every frame one is selected:
//   kind 0  3a8a70(world, mode 0..4)  terrain_raise / lower / level / level2 / level3
//   kind 1  3adf80(world)             terrain_demolish_trees
//   kind 2  3ccd70(world)             terrain_restore_topsoil
//   kind 3  2f94c0(world)             the single-tree tools (records with a species list)
// They act only while the button is held (exe+a54e90; the tree tool also on a
// press, exe+a54e91) from the cursor, the brush, the frame time, the level
// height taken at the press and their own state (held time, stroke cost, sound
// timers, the tree accumulator); 3a8a70 also asks C3D_INPUT for the press.
// What they change goes beyond the terrain's own edits: vegetation, the
// topsoil, money, trees.
//
// In lockstep a frame with the button down runs the tool with the button
// hidden (only the brush is drawn) and its inputs become a command: one sample
// of a stroke. Every PC runs the tool at the boundary with those inputs, the
// button set and the press answers forced; the tool's state comes from the
// stroke's first sample and from then on is what the replays leave, so both
// PCs evolve it alike.
#pragma once

#include "tesmio_plugin.h"

void BrushTools_Bind(const TsmHost* host);

// Startup: points 3a8a70's three press reads (3aa850, 3aaa58, 3aaaa9) at relays.
bool BrushTools_Install();

// False when a kind's routine differs (its tools are then put away in lockstep).
bool BrushTools_Ready(int kind);

// The tool stubs of the Late dispatcher call these instead of the routines.
unsigned __int64 BrushTools_Terrain(unsigned __int64 world, unsigned __int64 mode);
unsigned __int64 BrushTools_DemolishTrees(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 BrushTools_Topsoil(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 BrushTools_Tree(unsigned __int64 world, unsigned __int64 a2);

// The command (lockstep/commands).
bool BrushTools_ApplyCommand(const void* data, size_t size);
// A stroke's construction ledger row, written on every PC (see brush_tools.cpp).
bool BrushTools_ApplyLedger(const void* data, size_t size);

// True while a brush stroke is currently held/active on this PC.
bool BrushTools_IsActive();
// Test helper to mock time for rate limiting verification (0 = use real GetTickCount64).
void BrushTools_SetTestTimeMs(unsigned long long t);

