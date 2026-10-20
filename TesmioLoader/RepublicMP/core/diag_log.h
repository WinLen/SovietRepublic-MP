// SPDX-License-Identifier: GPL-3.0-only
// Comprehensive diagnostic logger for RepublicMP.
// Writes <GameDir>\RepublicMP_Diagnostics.log (and backs up the previous run to
// <GameDir>\RepublicMP_Diagnostics.prev.log), captures all Logf/UiLog/DebugLog
// output, periodic session/lockstep/SteamP2P telemetry, and crash stack traces.
#pragma once

#include "tesmio_plugin.h"

// Wraps TsmHost so every H->log (Logf) call across all modules is also written
// to RepublicMP_Diagnostics.log with millisecond timestamps and Thread IDs.
const TsmHost* DiagLog_InitAndWrapHost(const TsmHost* origHost);

// Logs system, module bases, PE timestamp, version, and CRC32 after hooks install.
void DiagLog_LogStartupSummary(void);

// Writes a formatted line to RepublicMP_Diagnostics.log.
void DiagLog_Printf(const char* category, const char* fmt, ...);

// Called every frame from SaveSync_Tick() to record state transitions and
// periodic 5-second session/lockstep/SteamP2P telemetry snapshots.
void DiagLog_FrameTick(void);

// Suppresses VectoredExceptionHandler crash logging inside guarded __try/__except probes.
void DiagLog_BeginSafeSeh(void);
void DiagLog_EndSafeSeh(void);
