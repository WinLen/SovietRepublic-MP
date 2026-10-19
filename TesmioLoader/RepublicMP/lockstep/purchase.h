// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: vehicle purchases as commands (by SoylentGreed).
//
// The purchase window's routine 80cc60 (window kinds 1, 2, 5, 6, 7; drawn and
// clicked from the interface on the render thread) buys only after its Buy
// press, read at 810943 from C3D_INPUT::GetMouseLeftPress. In a lockstep
// session that press reads "not pressed" on the clicking PC; what the window
// had at the press (the caller's registers: world r12, type r13, window r14,
// another button bl, prices xmm12 rubles / xmm13 dollars, availability xmm10)
// becomes a purchase command, and at the boundary every PC replays the Buy
// branch with the game's own routines (3e3f40 depot slot, 3e4a00 vehicle,
// 3fd5d0 coupling, 33f0 slot list, 2ab810 slot done, the licence and factory
// steps, the used-vehicle market). Sounds, messages and closing the window
// stay with the clicking PC.
#pragma once

#include "tesmio_plugin.h"

void Purchase_Bind(const TsmHost* host);

// The relay at 810943 (sim_patches): the press read with the caller's context
// one frame up. True: the game reads the press as it is.
bool Purchase_OnPress(bool pressed, const CONTEXT* caller);

// The command (lockstep/commands): every PC buys.
bool Purchase_ApplyCommand(const void* data, size_t size);
