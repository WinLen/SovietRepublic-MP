// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: building and world settings changed in windows as commands (by SoylentGreed).
//
// Windows write their settings straight into the objects, on the clicking PC
// only: a building's own words (the house window's options and limits), its
// storage slots (+970, 0xe0 each: the storage window's amounts, percentages,
// "store this item", the auto purchase amounts - the slot's own words, its
// +00 and +18 entries of 0x10 bytes and its +30 words) and the bottom menu's
// world settings block (+af8 auto-build with rubles, +afc with dollars, +b00
// immigrant auto-invite, +b04 auto-purchase of resources or goods, +b08 the
// citizens' option, +c798). Around the interface routine, on frames with a
// click (or Enter) over the interface, they are copied and compared
// afterwards; the changed words are put back at once and one command per
// object writes them on every PC at the boundary. Words the building settings
// sync carries with its own follow-up steps (session/settings_sync) are left
// to it, heap addresses are not carried, and words the interface changes
// without a click are learned from sampled frames and left out.
#pragma once

#include "tesmio_plugin.h"

void BuildingSwitches_Bind(const TsmHost* host);

// Render thread, around the interface routine. simBusy: the simulation's phase
// runs now (the comparison is skipped).
void BuildingSwitches_Before(bool simBusy);
void BuildingSwitches_After(bool simBusy);

// The command (lockstep/commands).
bool BuildingSwitches_ApplyCommand(const void* data, size_t size);

// Testable predicate: true if `offset` in a BUILDING struct (0x1420 bytes) is excluded as a view/vector field.
bool BuildingSwitches_IsExcludedBuildingOffset(unsigned offset);

