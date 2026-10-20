// SPDX-License-Identifier: GPL-3.0-only
// Comprehensive diagnostic logger for RepublicMP.
// Writes <GameDir>\RepublicMP_Diagnostics.log (and backs up the previous run to
// <GameDir>\RepublicMP_Diagnostics.prev.log), captures all Logf/UiLog/DebugLog
// output, periodic session/lockstep/SteamP2P telemetry, and crash stack traces.
#pragma once

#include "tesmio_plugin.h"
#include <string>

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

// Dumps an immediate full diagnostic snapshot, copies RepublicMP_Diagnostics.log
// to the user's Desktop, and opens Explorer selecting the file.
bool DiagLog_ExportToDesktop(std::string* outDesktopPath = nullptr);

// Dumps an immediate full diagnostic snapshot and returns the tail of the log
// (suitable for copying to the clipboard).
std::string DiagLog_GetClipboardReport(size_t maxBytes = 65536);

// Returns the full path to <GameDir>\RepublicMP_Diagnostics.log.
const char* DiagLog_GetLogFilePath(void);
