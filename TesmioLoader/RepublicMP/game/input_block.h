// SPDX-License-Identifier: GPL-3.0-only
// Real input blocking for the sync screen (ui/sync_block).
//
// Swallowing window messages (game/imgui_hook) is not enough: the game reads
// its input through C3D_INPUT in C3DDLL64 (bindings, mouse buttons, keys,
// axes, cursor position), which polls the devices itself. So the exe's
// imports of the C3D_INPUT query methods are hooked; each hook calls the
// original and, while the sync screen is up, replaces the answer with "no
// input": buttons/keys false, axes 0, mouse movement 0, and the cursor
// position frozen where it was when the block started (a cursor parked on
// the screen edge would otherwise keep edge-scrolling the camera).
//
// The game's own "left button held / pressed this frame" bytes
// (exe+0xA54E90/0xA54E91) are cleared every frame as well.
//
// Calls C3DDLL64 makes internally are not seen; everything the exe asks for
// goes through these imports.
#pragma once

#include "tesmio_plugin.h"

void InputBlock_Bind(const TsmHost* host);
bool InstallInputBlockHooks();

// True if world input is currently blocked by sync screen or unfocused window in co-op.
bool InputBlock_IsBlocked();

// Lockstep commands (main thread): while a command is made, the keys and
// bindings read up and the left button reads what the command wrote into the
// button bytes, so this PC's own keyboard and mouse cannot change it.
void InputBlock_BeginNeutral();
void InputBlock_EndNeutral();

// Per-frame (game thread, Present hook): clears the left-button bytes while
// blocked and tracks the start/end of a block.
void InputBlock_Tick();
