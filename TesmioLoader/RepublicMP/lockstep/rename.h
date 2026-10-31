// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: renaming a city/area or a crossroad as a command (by SoylentGreed).
//
// The rename window (801300) writes the typed name into its target's field
// with 5290(field, length, text) at 8018d0 (swprintf with the text as its
// format): a city/area's +0x10 (0x20 characters, the city window opens it), a
// crossroad's +4 (0x40, the junction window, which then sets +0xd0 = 1), and
// the buildings', vehicles' and line copies' names, which the interface
// switches and the line Accept carry already. A city's and a crossroad's name
// were written on the clicking PC only. game_calls takes the 5290 call: a
// field that is a listed city's +0x10 or a junction's +4 is not written there;
// the name travels (the city by its index in world +10348 and a check of its
// position, the crossroad by its first node), and every PC writes it at the
// boundary.
#pragma once

#include "tesmio_plugin.h"

void Rename_Bind(const TsmHost* host);

// Render thread, at the 5290 call (game_calls): true when the field is a city's or
// a crossroad's name and the name became a command (the window's write is skipped).
bool Rename_Capture(unsigned __int64 field, unsigned __int64 length, unsigned __int64 text);

// The command (lockstep/commands).
bool Rename_ApplyCommand(const void* data, size_t size);
bool Rename_ApplyDeleteCommand(const void* data, size_t size);
