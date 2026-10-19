// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the line window's Accept as a command (by SoylentGreed).
//
// The line window edits a working copy of its line (window +270); every edit
// of the copy is local. Accept (7d1d92..7d1e7e) copies it into the line
// (495830), merges equal lines and refreshes open line windows (4945a0), marks
// the line's vehicles unmeasured (+66c = -1), marks the line changed (22a6d0),
// gives its vehicles the new stops (497650) and names a private line (499600).
// In lockstep the copy is read at the 495830 call and sent as a command
// (stations and stop buildings by identity, signal nodes by network index,
// resource and vehicle type definitions by table index); the clicking PC's
// line stays as it was, and every PC rebuilds the copy and makes the block's
// calls on its own line at the boundary.
#pragma once

#include "tesmio_plugin.h"

void LineAccept_Bind(const TsmHost* host);

// Render thread, at the 495830 call (game_calls): true when the Accept became a command.
bool LineAccept_Capture(unsigned __int64 line, unsigned __int64 copy);
// At the 22a6d0 call of the same block: the vehicles' +66c the block's loop wrote are put back.
void LineAccept_RestoreMeasures();

// The command (lockstep/commands).
bool LineAccept_ApplyCommand(const void* data, size_t size);
