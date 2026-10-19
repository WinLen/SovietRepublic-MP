// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the cargo settings of stops and links as commands (by SoylentGreed).
//
// A vehicle's stops (vehicle +638) and a building's links (building +d38, link
// +10) are 0x198-byte records of two 0xc8-byte halves, loading and unloading:
// +0 byte, +4 the percentage, +8 the resource pairs (resource, value; 0x10
// each), scalars +20..+33, two int lists (+38, +50) and four definition lists
// (+68, +80, +98, +b0). The vehicle window and the building windows edit the
// halves through the half editor (7ec380) and the selection window, which push
// and remove the lists' entries in place, on the clicking PC only; the word
// comparison of the interface switches never saw the lists' contents.
// On frames with a click the halves of the vehicles and buildings shown in open
// windows are read before and after the interface routine. A changed half is
// put back at once (the game's half destroy 1513f0 and copy 204ef0 from a
// stand-in in mod memory) and sent with its resources and vehicle types by
// table index; every PC rebuilds the half the same way at the boundary, and a
// vehicle on a line refreshes the line manager for it (499790).
#pragma once

#include "tesmio_plugin.h"

void CargoSettings_Bind(const TsmHost* host);

// Render thread, around the interface routine. simBusy: the simulation's phase
// runs now (nothing is compared).
void CargoSettings_Before(bool simBusy);
void CargoSettings_After(bool simBusy);

// The command (lockstep/commands).
bool CargoSettings_ApplyCommand(const void* data, size_t size);
