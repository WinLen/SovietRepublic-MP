// SPDX-License-Identifier: GPL-3.0-only
// Determinism probe: a step-by-step trace of the simulation objects.
//
// The probe's samples (every 30 steps) only say that an object differs, and
// by then the cause is long gone. Run 1 records every vehicle, building and
// citizen, the world object and the game timer after every step (render-only words zeroed; a building with its storage
// slots and entries): in full after the first step, then only the dwords that
// changed. Run 2 replays that record step by step and compares its own objects
// with it. Words that differ the first time an object is compared are left
// over from the load (heap contents the game never initialised) and are left
// out, as are heap pointers. At the first step where anything else differs it
// logs the objects, the offsets and both runs' values, and from then on logs
// each offset that starts to differ once (a timeline). The world object and
// the timer are only ever watched that way.
#pragma once

#include "tesmio_plugin.h"

void ProbeTrace_Bind(const TsmHost* host);

// Before run 1: drops everything kept.
void ProbeTrace_Reset();
// Fuzz runs spool deltas to a delete-on-close temporary file, so the trace
// covers all requested ticks without raising its resident memory budget.
void ProbeTrace_SetDisk(bool enabled);
bool ProbeTrace_Complete(unsigned long long steps);
bool ProbeTrace_Different();
unsigned long long ProbeTrace_FirstDifference();

// Main thread, after Early and Late of tick `step` (1-based) of probe run
// `run` (0 or 1).
void ProbeTrace_Step(int run, unsigned long long step);

// After run 2: logs a line if the trace found no difference.
void ProbeTrace_Finish();
