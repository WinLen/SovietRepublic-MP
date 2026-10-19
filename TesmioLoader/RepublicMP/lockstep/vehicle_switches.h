// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: vehicle settings changed in windows as commands (by SoylentGreed).
//
// The vehicle window's checkboxes, limits and sliders, the per-stop load and
// unload settings (stop records +638) and the cargo limits of "Advanced setup"
// (cargo records +3b8) are written by the interface straight into the
// vehicle, on the clicking PC only. Around the interface routine, on frames
// with a click (or Enter) over the interface, every vehicle and those records
// are copied and compared afterwards; the changed words are put back at once
// and one command per vehicle writes them on every PC at the boundary. Words
// that hold heap addresses are not carried (reported). Words the interface
// changes on frames without a click are its own noise: they are learned from
// sampled frames, left out and reported once (they would change one PC only).
#pragma once

#include "tesmio_plugin.h"

void VehicleSwitches_Bind(const TsmHost* host);

// Render thread, around the interface routine. simBusy: the simulation's phase
// runs now (the comparison is skipped).
void VehicleSwitches_Before(bool simBusy);
void VehicleSwitches_After(bool simBusy);

// The command (lockstep/commands).
bool VehicleSwitches_ApplyCommand(const void* data, size_t size);
