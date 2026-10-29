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

// Remote client log upload to Host (<GameDir>\RemoteLogs\<Reason>_<YYYYMMDD_HHMMSS>\)
#include <vector>
#include <stdint.h>
bool DiagLog_BuildClientLogBundle(std::vector<unsigned char>& outPayload, const char* reason);
bool DiagLog_SaveRemoteClientLogBundle(const void* data, size_t size);
bool DiagLog_HasPrevCrashOrDesyncLog(void);

// Vehicle synchronization & DLC protection telemetry
struct VehicleSyncDiagStats {
    uint64_t purchasesAttempted;
    uint64_t purchasesMade;
    uint64_t purchasesBlockedDlc;
    uint64_t purchasesBlockedMod;
    uint64_t purchasesBlockedMoneyOrDepot;
    uint64_t scriptNameRemapped;
    uint64_t scriptNameResolveFails;
    uint64_t vehiclesSold;
    uint32_t countRoad;
    uint32_t countRail;
    uint32_t countWater;
    uint32_t countAir;
    uint32_t countSpecial;
};

void Purchase_GetDiagStats(VehicleSyncDiagStats* outStats);
void Purchase_RecordDiagSale(unsigned cat);
void Purchase_ResetDiagStatsForTest();

