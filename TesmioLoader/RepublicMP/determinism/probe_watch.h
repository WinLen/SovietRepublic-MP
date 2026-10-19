// SPDX-License-Identifier: GPL-3.0-only
// Determinism probe: hardware write watch on a few words.
//
// The step trace says which word of which object splits first and at which
// step, but not who wrote it. This puts the CPU's debug registers on four
// fields of the citizen nearest to a spot, in every thread of the game, and
// records each write: the step, the citizen, the thread, the writing
// instruction and its callers (exe offsets) and the new value. After run 2 it
// logs where the two runs' write sequences first part. Off by default
// (WATCH_ON in probe_watch.cpp).
#pragma once

#include "tesmio_plugin.h"

void ProbeWatch_Bind(const TsmHost* host);

// Before run 1: drops everything kept.
void ProbeWatch_Reset();

// Main thread, right after simulation step `step` (1-based) of probe run
// `run` (0 or 1); `last` = this was the run's last step.
void ProbeWatch_Step(int run, unsigned long long step, bool last);

// After run 2: logs the comparison.
void ProbeWatch_Finish();
