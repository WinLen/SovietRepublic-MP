// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the buildings' specific sources as commands (by SoylentGreed).
//
// A building's source slots (+f78..+f80, 0x10 each: the source building at +0,
// the slot's kind at +8) name the buildings its vehicles load at or haul to (a
// waste dump, a construction office's suppliers). building_addsource (1d51a0,
// a Late tool) writes the picked building into the slots of the window's
// building (world +d298); building_copysource (1d6820, a Late tool) copies
// source slots between buildings; the source windows and the "Cancel all source
// buildings" popup clear them. The slots live on the heap, so the word
// comparison of the interface switches never saw them: they changed on the
// clicking PC only.
// Around the tool and around the interface routine on frames with a click, the
// slots of the window's building and of the buildings in the world are read
// before and after; a changed slot is put back at once, and every PC writes it
// (the source by identity) at the boundary.
#pragma once

#include "tesmio_plugin.h"

#define SOURCE_SLOTS_MAX_BUILDINGS 16384

void SourceSlots_Bind(const TsmHost* host);

// The tool's routine is the checked one (the tool is put away otherwise).
bool SourceSlots_Ready();

// The tool stub's targets for building_addsource (1d51a0) and building_copysource (1d6820).
unsigned __int64 SourceSlots_AddSourceTool(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 SourceSlots_CopySourceTool(unsigned __int64 world, unsigned __int64 a2);
void SourceSlots_SetToolCallbackForTest(unsigned __int64 (*fn)(unsigned __int64 world, unsigned __int64 a2));

// Render thread, around the interface routine. simBusy: the simulation's phase
// runs now (nothing is compared).
void SourceSlots_Before(bool simBusy);
void SourceSlots_After(bool simBusy);

// The command (lockstep/commands).
bool SourceSlots_ApplyCommand(const void* data, size_t size);
